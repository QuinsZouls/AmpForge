#include "JackIo.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(__linux__)
# include <dlfcn.h>
#endif

// Why this file never includes <jack/jack.h> nor links libjack: the UI
// library it belongs to is also cross-compiled for Windows and macOS
// (cmake/toolchain-mingw64.cmake), where the JACK headers simply do not
// exist - linking against them would break those builds. So the handful of
// libjack entry points we need are declared here by hand and resolved at
// runtime through dlopen()/dlsym(), exactly like DPF itself does in
// distrho/src/jackbridge/JackBridge.cpp. On top of that the standalone's
// own client already keeps libjack mapped, so opening it again costs
// nothing and never touches the audio thread.

namespace ampforge {

namespace {

// HOME is the right variable on Linux/macOS, but is typically unset for a
// native Windows process - same fallback order as ChainUI::getUserHomeDir().
std::string getUserHomeDir()
{
    if (const char* home = std::getenv("HOME"))
        return home;
    if (const char* profile = std::getenv("USERPROFILE"))
        return profile;
    if (const char* appdata = std::getenv("APPDATA"))
        return appdata;
    return ".";
}

std::string getAudioIoConfigPath()
{
    const std::string dir = getUserHomeDir() + "/.config/ampforge";
    // The std::error_code overload: a read-only or otherwise unusable HOME
    // must degrade to "no settings file", never throw out of the UI thread.
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir + "/audio_io.txt";
}

std::string trim(const std::string& text)
{
    const std::string::size_type first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return std::string();
    const std::string::size_type last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

} // namespace

#if defined(__linux__)

namespace {

// JACK's public constants, hand-copied from <jack/types.h> (see the file
// comment above for why we can't include it). The values are part of JACK's
// stable ABI.
enum JackConstant
{
    kJackNoStartServer  = 0x01,
    kJackPortIsInput    = 0x1,
    kJackPortIsOutput   = 0x2,
    kJackPortIsPhysical = 0x4,
    kJackPortIsTerminal = 0x10
};

constexpr const char* kAuxClientName = "AmpForge-IO";
constexpr const char* kAudioPortType = "32 bit float mono audio";
constexpr const char* kStandaloneInputPort  = "audio_in_1";
constexpr const char* kStandaloneOutputPort = "audio_out_1";
constexpr const char* kDpfClientName = "AmpForge";

// Opaque JACK types: we only ever hand these pointers back to libjack.
struct jack_client_t;
struct jack_port_t;

// C-ABI entry points, resolved with dlsym(). Enums are passed as int and
// flags as unsigned long, matching the C prototypes exactly.
typedef jack_client_t* (*JackClientOpenFn)(const char*, int, int*, ...);
typedef int            (*JackClientCloseFn)(jack_client_t*);
typedef char*          (*JackGetClientNameFn)(jack_client_t*);
typedef const char**   (*JackGetPortsFn)(jack_client_t*, const char*, const char*, unsigned long);
typedef jack_port_t*   (*JackPortByNameFn)(jack_client_t*, const char*);
typedef const char*    (*JackPortNameFn)(jack_port_t*);
typedef unsigned long  (*JackPortFlagsFn)(jack_port_t*);
typedef const char*    (*JackPortTypeFn)(jack_port_t*);
typedef const char**   (*JackPortGetAllConnectionsFn)(jack_client_t*, jack_port_t*);
typedef int            (*JackConnectFn)(jack_client_t*, const char*, const char*);
typedef int            (*JackDisconnectFn)(jack_client_t*, const char*, const char*);
typedef unsigned int   (*JackGetSampleRateFn)(jack_client_t*);
typedef unsigned int   (*JackGetBufferSizeFn)(jack_client_t*);
typedef void           (*JackFreeFn)(void*);
typedef void           (*JackOnShutdownFn)(jack_client_t*, void (*)(void*), void*);

// Splits a full JACK port name into client and short port name. Splitting
// on the LAST ':' is the only safe choice: client names are free-form
// (spaces, dots and even ':' are allowed) while a port's short name
// practically never contains one.
void splitPortName(const std::string& fullName, std::string& client, std::string& port)
{
    const std::string::size_type colon = fullName.rfind(':');
    if (colon == std::string::npos)
    {
        client.clear();
        port = fullName;
        return;
    }
    client = fullName.substr(0, colon);
    port = fullName.substr(colon + 1);
}

} // namespace

struct JackIo::Impl
{
    void* lib = nullptr;
    bool libTried = false;
    std::string error;

    JackClientOpenFn           clientOpen = nullptr;
    JackClientCloseFn          clientClose = nullptr;
    JackGetClientNameFn        getClientName = nullptr;
    JackGetPortsFn             getPorts = nullptr;
    JackPortByNameFn           portByName = nullptr;
    JackPortNameFn             portName = nullptr;
    JackPortFlagsFn            portFlags = nullptr;
    JackPortTypeFn             portType = nullptr;
    JackPortGetAllConnectionsFn portGetAllConnections = nullptr;
    JackConnectFn              connectPort = nullptr;
    JackDisconnectFn           disconnectPort = nullptr;
    JackGetSampleRateFn        getSampleRate = nullptr;
    JackGetBufferSizeFn        getBufferSize = nullptr;
    JackFreeFn                 freeMem = nullptr;
    JackOnShutdownFn           onShutdown = nullptr;

    jack_client_t* client = nullptr;

    // Set from inside the JACK shutdown callback, which runs on a JACK
    // thread - so it is the only thing that callback is allowed to touch.
    std::atomic<bool> serverDown{false};

    // pluginClient() is called a handful of times per repaint; walking the
    // whole graph each time is wasteful, so the answer is memoised here.
    mutable std::string cachedPluginClient;

    ~Impl();

    // Runs on a JACK thread: flip the flag and nothing else. Touching the
    // UI, strings or the client handle from here would be a data race.
    static void shutdownCallback(void* arg)
    {
        static_cast<std::atomic<bool>*>(arg)->store(true);
    }

    template<typename Fn>
    bool resolveSymbol(const char* name, Fn& out)
    {
        out = reinterpret_cast<Fn>(reinterpret_cast<uintptr_t>(::dlsym(lib, name)));
        if (out != nullptr)
            return true;
        error = std::string("libjack is missing the symbol '") + name + "'";
        return false;
    }

    bool ensureLibrary()
    {
        if (lib != nullptr)
            return true;
        // A missing libjack is a permanent condition for this process; don't
        // retry a failing dlopen() from every repaint.
        if (libTried)
            return false;
        libTried = true;

        lib = ::dlopen("libjack.so.0", RTLD_NOW | RTLD_LOCAL);
        if (lib == nullptr)
            lib = ::dlopen("libjack.so", RTLD_NOW | RTLD_LOCAL);
        if (lib == nullptr)
        {
            const char* reason = ::dlerror();
            error = std::string("could not load libjack: ") + (reason != nullptr ? reason : "unknown error");
            return false;
        }

        // Note: the handle is deliberately never dlclose()d. DPF's own
        // standalone client keeps the same library mapped for the whole
        // process lifetime, so unloading it here would be pointless at best.
        return resolveSymbol("jack_client_open", clientOpen)
            && resolveSymbol("jack_client_close", clientClose)
            && resolveSymbol("jack_get_client_name", getClientName)
            && resolveSymbol("jack_get_ports", getPorts)
            && resolveSymbol("jack_port_by_name", portByName)
            && resolveSymbol("jack_port_name", portName)
            && resolveSymbol("jack_port_flags", portFlags)
            && resolveSymbol("jack_port_type", portType)
            && resolveSymbol("jack_port_get_all_connections", portGetAllConnections)
            && resolveSymbol("jack_connect", connectPort)
            && resolveSymbol("jack_disconnect", disconnectPort)
            && resolveSymbol("jack_get_sample_rate", getSampleRate)
            && resolveSymbol("jack_get_buffer_size", getBufferSize)
            && resolveSymbol("jack_free", freeMem)
            && resolveSymbol("jack_on_shutdown", onShutdown);
    }

    void closeClient()
    {
        // After a shutdown callback the handle is dead for some servers;
        // touching it then is undefined behaviour, so drop it instead.
        if (client != nullptr && !serverDown.load() && clientClose != nullptr)
            clientClose(client);
        client = nullptr;
        serverDown.store(false);
        cachedPluginClient.clear();
    }

    bool ensureOpen()
    {
        if (!ensureLibrary())
            return false;

        // The callback fired since the last call: whatever we cached about
        // the old client is stale, so start over (the server may well be
        // back up already - PipeWire restarts are routine).
        if (serverDown.load())
        {
            client = nullptr;
            serverDown.store(false);
            cachedPluginClient.clear();
        }

        if (client != nullptr)
            return true;

        int status = 0;
        // JackNoStartServer: never spawn a server behind the user's back
        // (jack_client_open() would otherwise block for seconds trying).
        client = clientOpen(kAuxClientName, kJackNoStartServer, &status);
        if (client == nullptr)
        {
            error = std::string("could not open the JACK client '") + kAuxClientName + "'";
            return false;
        }

        if (onShutdown != nullptr)
            onShutdown(client, &shutdownCallback, &serverDown);
        return true;
    }

    void freePortList(const char** ports)
    {
        if (ports != nullptr && freeMem != nullptr)
            freeMem(const_cast<void*>(static_cast<const void*>(ports)));
    }

    // Ports currently linked to a full port name, as full names.
    std::vector<std::string> connections(const std::string& fullName)
    {
        std::vector<std::string> result;
        if (client == nullptr)
            return result;

        jack_port_t* port = portByName(client, fullName.c_str());
        if (port == nullptr)
            return result;

        const char** linked = portGetAllConnections(client, port);
        if (linked == nullptr)
            return result;
        for (int i = 0; linked[i] != nullptr; ++i)
            result.push_back(linked[i]);
        freePortList(linked);
        return result;
    }

    // isSource tells jack_disconnect() which end of the edge our port is on
    // - it wants (source, destination) in graph order, not just "these two".
    bool clearConnections(const std::string& fullName, bool isSource)
    {
        const std::vector<std::string> linked = connections(fullName);
        for (const std::string& other : linked)
        {
            const int rc = isSource
                ? disconnectPort(client, fullName.c_str(), other.c_str())
                : disconnectPort(client, other.c_str(), fullName.c_str());
            if (rc != 0)
            {
                error = "could not disconnect '" + other + "' from '" + fullName + "'";
                return false;
            }
        }
        return true;
    }

    bool makeConnection(const std::string& source, const std::string& destination)
    {
        if (connectPort(client, source.c_str(), destination.c_str()) != 0)
        {
            error = "could not connect '" + source + "' to '" + destination + "'";
            return false;
        }
        return true;
    }

    // The DPF standalone's client, or "" when we're not running standalone.
    std::string pluginClientName()
    {
        if (client == nullptr)
            return std::string();

        if (!cachedPluginClient.empty())
        {
            // The cached name goes stale when the standalone exits. One
            // jack_port_by_name() probe is much cheaper than re-walking the
            // graph, and it answers exactly that question.
            const std::string probe = cachedPluginClient + ":" + kStandaloneInputPort;
            if (portByName(client, probe.c_str()) != nullptr)
                return cachedPluginClient;
            cachedPluginClient.clear();
        }

        std::string found;
        // No client/type/flag filter: the standalone's ports may be of a
        // type we didn't ask for, and we need to see MIDI ports too to
        // recognise the client that owns them.
        const char** ports = getPorts(client, nullptr, nullptr, 0);
        if (ports != nullptr)
        {
            for (int i = 0; ports[i] != nullptr && found.empty(); ++i)
            {
                std::string owner;
                std::string shortName;
                splitPortName(ports[i], owner, shortName);
                if (owner == kAuxClientName || shortName != kStandaloneInputPort)
                    continue;
                // DPF passes the plugin name as the client name; a second
                // instance gets a "-01"/"-02" suffix added by JACK.
                if (owner == kDpfClientName
                    || owner.compare(0, std::string(kDpfClientName).size() + 1, std::string(kDpfClientName) + "-") == 0)
                    found = owner;
            }
            freePortList(ports);
        }

        cachedPluginClient = found;
        return found;
    }

    std::vector<JackIo::JackPortInfo> candidates(unsigned long flags)
    {
        std::vector<JackIo::JackPortInfo> unphysical;
        std::vector<JackIo::JackPortInfo> physical;
        if (!ensureOpen())
            return unphysical;

        const std::string self = pluginClientName();
        const char** ports = getPorts(client, nullptr, kAudioPortType, flags);
        if (ports != nullptr)
        {
            for (int i = 0; ports[i] != nullptr; ++i)
            {
                const std::string rawName = ports[i];
                JackIo::JackPortInfo info;
                splitPortName(rawName, info.client, info.port);

                // Our own clients are never valid targets: connecting
                // audio_out_1 back into audio_in_1 is a one-click feedback
                // loop, and the aux client only exists to list things.
                if (info.client == kAuxClientName || (!self.empty() && info.client == self))
                    continue;

                jack_port_t* port = portByName(client, rawName.c_str());
                if (port == nullptr)
                    continue;

                // The type pattern above already drops MIDI on every server
                // we care about, but pattern matching is implementation
                // defined - double-check the real type.
                const char* type = portType(port);
                if (type == nullptr || std::string(type).find("audio") == std::string::npos)
                    continue;

                const char* canonical = portName(port);
                info.name = (canonical != nullptr && canonical[0] != '\0') ? canonical : rawName;
                info.isPhysical = (portFlags(port) & kJackPortIsPhysical) != 0;
                (info.isPhysical ? physical : unphysical).push_back(info);
            }
            freePortList(ports);
        }

        // Physical ports first: the graph is full of monitors and effect
        // loops (Easy Effects etc.) that a guitarist never wants to pick,
        // while the real interface is exactly the physical set. Only when
        // there is no physical port at all (no soundcard, a pure virtual
        // setup) do we expose everything.
        std::vector<JackIo::JackPortInfo>& selected = physical.empty() ? unphysical : physical;
        std::sort(selected.begin(), selected.end(),
                  [](const JackIo::JackPortInfo& a, const JackIo::JackPortInfo& b) { return a.name < b.name; });
        return selected;
    }
};

JackIo::Impl::~Impl()
{
    closeClient();
}

JackIo::JackIo()
    : impl(new Impl())
{
}

JackIo::~JackIo()
{
    delete impl;
    impl = nullptr;
}

bool JackIo::available()
{
    return impl->ensureOpen();
}

void JackIo::closeClient()
{
    impl->closeClient();
}

const std::string& JackIo::lastError() const
{
    return impl->error;
}

bool JackIo::isStandalone()
{
    if (!impl->ensureOpen())
        return false;
    return !impl->pluginClientName().empty();
}

std::string JackIo::pluginClient()
{
    if (!impl->ensureOpen())
        return std::string();
    return impl->pluginClientName();
}

std::vector<JackIo::JackPortInfo> JackIo::inputCandidates()
{
    return impl->candidates(kJackPortIsOutput);
}

std::vector<JackIo::JackPortInfo> JackIo::outputCandidates()
{
    return impl->candidates(kJackPortIsInput);
}

JackIo::IoState JackIo::queryState()
{
    IoState state;
    if (!impl->ensureOpen())
        return state;

    state.serverRunning = true;
    state.sampleRate = static_cast<int>(impl->getSampleRate(impl->client));
    state.bufferSize = static_cast<int>(impl->getBufferSize(impl->client));

    const std::string clientName = impl->pluginClientName();
    if (clientName.empty())
        return state;

    state.standalone = true;
    state.client = clientName;

    const std::vector<std::string> inputs = impl->connections(clientName + ":" + kStandaloneInputPort);
    if (!inputs.empty())
        state.input = inputs[0];

    const std::vector<std::string> outputs = impl->connections(clientName + ":" + kStandaloneOutputPort);
    if (!outputs.empty())
        state.outputL = outputs[0];
    if (outputs.size() > 1)
        state.outputR = outputs[1];

    return state;
}

bool JackIo::setInput(const std::string& port)
{
    // Start from a clean slate: lastError() must describe the call that just
    // failed, not one from three clicks ago.
    impl->error.clear();
    if (!impl->ensureOpen())
        return false;

    const std::string clientName = impl->pluginClientName();
    if (clientName.empty())
    {
        impl->error = "AmpForge is not running as a standalone JACK client";
        return false;
    }

    const std::string inputPort = clientName + ":" + kStandaloneInputPort;
    // audio_in_1 is the graph edge's destination.
    if (!impl->clearConnections(inputPort, false))
        return false;

    if (port.empty())
        return true;

    return impl->makeConnection(port, inputPort);
}

bool JackIo::setOutputs(const std::string& l, const std::string& r)
{
    impl->error.clear();
    if (!impl->ensureOpen())
        return false;

    const std::string clientName = impl->pluginClientName();
    if (clientName.empty())
    {
        impl->error = "AmpForge is not running as a standalone JACK client";
        return false;
    }

    const std::string outputPort = clientName + ":" + kStandaloneOutputPort;
    // audio_out_1 is the graph edge's source.
    if (!impl->clearConnections(outputPort, true))
        return false;

    if (l.empty())
        return true;

    if (!impl->makeConnection(outputPort, l))
        return false;
    // Mono out feeding the same destination twice is not a stereo pair.
    if (!r.empty() && r != l && !impl->makeConnection(outputPort, r))
        return false;
    return true;
}

void JackIo::disconnectAll()
{
    impl->error.clear();
    if (!impl->ensureOpen())
        return;

    const std::string clientName = impl->pluginClientName();
    if (clientName.empty())
        return;

    impl->clearConnections(clientName + ":" + kStandaloneInputPort, false);
    impl->clearConnections(clientName + ":" + kStandaloneOutputPort, true);
}

#else // !defined(__linux__)

// macOS uses CoreAudio for its standalone and Windows has no JACK at all;
// both still compile this file into the UI library, so everything collapses
// to the "nothing is available" answers.
struct JackIo::Impl
{
    std::string error = "JACK routing is only supported on Linux";
};

JackIo::JackIo()
    : impl(new Impl())
{
}

JackIo::~JackIo()
{
    delete impl;
    impl = nullptr;
}

bool JackIo::available()
{
    return false;
}

void JackIo::closeClient()
{
}

const std::string& JackIo::lastError() const
{
    return impl->error;
}

bool JackIo::isStandalone()
{
    return false;
}

std::string JackIo::pluginClient()
{
    return std::string();
}

std::vector<JackIo::JackPortInfo> JackIo::inputCandidates()
{
    return std::vector<JackIo::JackPortInfo>();
}

std::vector<JackIo::JackPortInfo> JackIo::outputCandidates()
{
    return std::vector<JackIo::JackPortInfo>();
}

JackIo::IoState JackIo::queryState()
{
    return IoState();
}

bool JackIo::setInput(const std::string& port)
{
    (void)port;
    return false;
}

bool JackIo::setOutputs(const std::string& l, const std::string& r)
{
    (void)l;
    (void)r;
    return false;
}

void JackIo::disconnectAll()
{
}

#endif // defined(__linux__)

bool AudioIoConfig::isEmpty() const
{
    return input.empty() && outputL.empty() && outputR.empty();
}

AudioIoConfig loadAudioIoConfig()
{
    AudioIoConfig config;

    const std::string path = getAudioIoConfigPath();
    std::ifstream f(path);
    if (!f.is_open())
        return config;

    std::string line;
    while (std::getline(f, line))
    {
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#')
            continue;

        const std::string::size_type equals = trimmed.find('=');
        if (equals == std::string::npos)
            continue;

        const std::string key = trim(trimmed.substr(0, equals));
        const std::string value = trim(trimmed.substr(equals + 1));
        if (key == "input")
            config.input = value;
        else if (key == "output_l")
            config.outputL = value;
        else if (key == "output_r")
            config.outputR = value;
        // Unknown keys are ignored on purpose: an older build must keep
        // working with a file a newer one wrote.
    }
    return config;
}

void saveAudioIoConfig(const AudioIoConfig& config)
{
    const std::string path = getAudioIoConfigPath();
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open())
        return;

    f << "# AmpForge audio I/O - written by the plugin; edit while the plugin is closed.\n"
      << "input=" << config.input << "\n"
      << "output_l=" << config.outputL << "\n"
      << "output_r=" << config.outputR << "\n";
}

} // namespace ampforge
