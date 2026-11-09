#include "JackIo.hpp"

// For DISTRHO_PLUGIN_NAME: the standalone's JACK client name is whatever DPF
// passes to jack_client_open(), i.e. the plugin name. Hard-coding "AmpForge"
// here would silently stop matching the day the plugin is renamed.
// This header is pure #define's, so it pulls in no DPF dependency (in
// particular not DistrhoUI.hpp, which this translation unit never sees).
#include "DistrhoPluginInfo.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(__linux__)
# include <dlfcn.h>
// ::getpid(), needed to tell our own JACK client apart from another
// process's client that happens to carry the same name (see J2 note in
// Impl::pluginClientName()).
# include <unistd.h>
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
// The name DPF's standalone gives its JACK client is the plugin name.
constexpr const char* kDpfClientName = DISTRHO_PLUGIN_NAME;

// Opaque JACK types: we only ever hand these pointers back to libjack.
struct jack_client_t;
struct jack_port_t;

// C-ABI entry points, resolved with dlsym(). Enums are passed as int and
// flags as unsigned long, matching the C prototypes exactly - including the
// const-ness of every pointer argument and of the port pointer itself, so
// these signatures really are the ones declared in <jack/jack.h>.
typedef jack_client_t* (*JackClientOpenFn)(const char*, int, int*, ...);
typedef int            (*JackClientCloseFn)(jack_client_t*);
typedef int            (*JackGetClientPidFn)(const char*);
typedef const char**   (*JackGetPortsFn)(jack_client_t*, const char*, const char*, unsigned long);
typedef jack_port_t*   (*JackPortByNameFn)(jack_client_t*, const char*);
typedef const char*    (*JackPortNameFn)(const jack_port_t*);
typedef int            (*JackPortFlagsFn)(const jack_port_t*);
typedef const char*    (*JackPortTypeFn)(const jack_port_t*);
typedef const char**   (*JackPortGetAllConnectionsFn)(const jack_client_t*, const jack_port_t*);
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
    // True only once every *required* symbol resolved. The handle alone is
    // not proof of a usable library: a libjack that dlopen()s fine but lacks
    // one entry point must never be reported as ready, or the callers run
    // straight into a null function pointer.
    bool libOk = false;
    std::string error;

    JackClientOpenFn           clientOpen = nullptr;
    JackClientCloseFn          clientClose = nullptr;
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
    // Optional: older/embedded JACK servers may not export it. Its absence
    // disables the PID cross-check below, nothing else.
    JackGetClientPidFn         getClientPid = nullptr;

    jack_client_t* client = nullptr;

    // Set from inside the JACK shutdown callback, which runs on a JACK
    // thread - so it is the only thing that callback is allowed to touch.
    std::atomic<bool> serverDown{false};

    // Walking the whole graph is far too expensive to redo on a mouse move,
    // and pluginClientName() is reached from the mouse handlers and from the
    // startup poll in uiIdle() (never from a paint pass), so the answer is
    // memoised here.
    mutable std::string cachedPluginClient;

    ~Impl();

    // Runs on a JACK thread: flip the flag and nothing else. Touching the
    // UI, strings or the client handle from here would be a data race.
    static void shutdownCallback(void* arg)
    {
        static_cast<std::atomic<bool>*>(arg)->store(true);
    }

    // required=false keeps the symbol out of ensureLibrary()'s success
    // condition and leaves the previous error text alone: a missing optional
    // entry point degrades one feature, it does not make libjack unusable.
    template<typename Fn>
    bool resolveSymbol(const char* name, Fn& out, bool required = true)
    {
        out = reinterpret_cast<Fn>(reinterpret_cast<uintptr_t>(::dlsym(lib, name)));
        if (out != nullptr)
            return true;
        if (required)
            error = std::string("libjack is missing the symbol '") + name + "'";
        return false;
    }

    bool ensureLibrary()
    {
        // The handle is not the answer: lib is set as soon as dlopen()
        // succeeds, so a library that resolved only half of its entry points
        // would otherwise wave the caller through to a null pointer.
        if (libOk)
            return true;
        // A missing libjack is a permanent condition for this process; don't
        // retry a failing dlopen() from every UI interaction. Same for a
        // library that failed to resolve: dlopen() is not attempted a second
        // time, the (still valid) handle is reused as a plain memory
        // reference.
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
        libOk = resolveSymbol("jack_client_open", clientOpen)
            && resolveSymbol("jack_client_close", clientClose)
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
        if (!libOk)
            return false;

        // Optional, resolved last and outside the condition above: when it is
        // missing we fall back to matching on the port name alone.
        resolveSymbol("jack_get_client_pid", getClientPid, false);
        return true;
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

    // True when `owner` is a JACK client of *this* process.
    //
    // A matching name and port are not enough to conclude "we are the
    // standalone": this UI also ships inside LV2/VST3/CLAP, so a plugin
    // loaded in a DAW sees the standalone's client 'AmpForge' on the very
    // same graph - and would happily rewrite its routing and its
    // ~/.config/ampforge/audio_io.txt. Two standalone instances collide the
    // same way. The owning PID is what actually answers "is that mine?".
    //
    // When the server cannot tell (older JACK builds do not export the
    // symbol), we keep the pre-PID behaviour and match on the name. A
    // *resolved* symbol answering 0 means "unknown owner" (in practice a
    // client that just vanished, or a server that does not track pids), and
    // that is never our own PID: an owner we cannot prove is ours is treated
    // as somebody else's, so we never reroute a graph that is not provably
    // our own.
    bool isClientOfThisProcess(const std::string& owner)
    {
        if (getClientPid == nullptr)
            return true;
        return getClientPid(owner.c_str()) == static_cast<int>(::getpid());
    }

    // The DPF client owned by this process, or "" when we're not the
    // standalone (running inside a DAW, or no such client at all).
    std::string pluginClientName()
    {
        if (client == nullptr)
            return std::string();

        if (!cachedPluginClient.empty())
        {
            // The cached name goes stale when the standalone exits, or when
            // another process grabs the freed name. One jack_port_by_name()
            // probe is much cheaper than re-walking the graph, and the PID
            // re-check keeps the memoised name honest.
            const std::string probe = cachedPluginClient + ":" + kStandaloneInputPort;
            if (portByName(client, probe.c_str()) != nullptr
                && isClientOfThisProcess(cachedPluginClient))
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
                if (owner != kDpfClientName
                    && owner.compare(0, std::string(kDpfClientName).size() + 1, std::string(kDpfClientName) + "-") != 0)
                    continue;
                // Cheap name test passed: only now pay for the round trip
                // that proves the client belongs to us.
                if (!isClientOfThisProcess(owner))
                    continue;
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
            return std::vector<JackIo::JackPortInfo>();

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

        // Physical (device) ports first, then the virtual ones, each block
        // sorted by name. Nothing is hidden: the real interface is the
        // physical set and belongs at the top, but Easy Effects, OBS,
        // monitor_* sinks and net/jacktrip bridges are legitimate targets
        // too. Dropping them also used to turn a perfectly valid saved route
        // into "(not found)" the moment a USB interface was plugged in.
        const auto byName = [](const JackIo::JackPortInfo& a, const JackIo::JackPortInfo& b) { return a.name < b.name; };
        std::sort(physical.begin(), physical.end(), byName);
        std::sort(unphysical.begin(), unphysical.end(), byName);

        std::vector<JackIo::JackPortInfo> all;
        all.reserve(physical.size() + unphysical.size());
        all.insert(all.end(), physical.begin(), physical.end());
        all.insert(all.end(), unphysical.begin(), unphysical.end());
        return all;
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

// Contract (deliberate, also spelled out in JackIo.hpp):
//   l empty           -> everything is disconnected and `r` is IGNORED. A
//                        right channel alone is not a pair, so there is
//                        nothing to connect; passing a stale output_r along
//                        with an empty output_l must not resurrect it.
//   r empty, r == l   -> one mono connection to `l`.
//   otherwise         -> two connections, `l` and `r`.
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

    // No left channel means no pair: `r` is dropped on purpose, see above.
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

bool saveAudioIoConfig(const AudioIoConfig& config)
{
    const std::string path = getAudioIoConfigPath();
    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open())
        return false;

    f << "# AmpForge audio I/O - written by the plugin; edit while the plugin is closed.\n"
      << "input=" << config.input << "\n"
      << "output_l=" << config.outputL << "\n"
      << "output_r=" << config.outputR << "\n";

    // A full disk or a quota is only reported once the buffer is flushed, so
    // close() and check before claiming success: the caller has to be able
    // to say "could not save" instead of a "Connected" that vanishes on the
    // next start.
    f.close();
    return !f.fail();
}

} // namespace ampforge
