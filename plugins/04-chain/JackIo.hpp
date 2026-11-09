#ifndef AMPFORGE_JACKIO_HPP_INCLUDED
#define AMPFORGE_JACKIO_HPP_INCLUDED

#include <string>
#include <vector>

namespace ampforge {

// Audio port routing for the standalone build.
//
// The DPF standalone opens a JACK client with a single mono input
// ("audio_in_1") and a single mono output ("audio_out_1") but never
// connects them to anything, so without this the user has to reach for
// pw-link/jack-plumber. This class is the core behind the Settings panel's
// input/output pickers: it enumerates the ports we may read from / write
// to, reports the current connections, and (re)connects them.
//
// Everything here talks to libjack through dlopen() + dlsym() instead of
// linking it - see JackIo.cpp.
class JackIo
{
public:
    struct JackPortInfo
    {
        std::string name;       // full name, "Ryzen HD Audio Controller Analog Stereo:capture_FL"
        std::string client;     // "Ryzen HD Audio Controller Analog Stereo"
        std::string port;       // "capture_FL"
        bool isPhysical = false;
    };

    JackIo();
    ~JackIo();
    JackIo(const JackIo&) = delete;
    JackIo& operator=(const JackIo&) = delete;

    // dlopen of libjack + (idempotent) opening of our auxiliary client.
    bool available();
    void closeClient();
    const std::string& lastError() const;

    // Detects "we are running as the standalone": looks for a JACK client,
    // owned by THIS process and exposing the "audio_in_1" port that DPF's
    // standalone client registers. Inside a DAW our code lives in the host's
    // process, whose own clients have no such ports -> false. The owning PID
    // is what makes the answer trustworthy: a DAW instance also sees the
    // standalone's 'AmpForge' client on the graph, and must not mistake it
    // for its own.
    bool isStandalone();
    std::string pluginClient();     // "AmpForge" (or "AmpForge-01"...); "" if not standalone

    // Candidates for INPUT: ports we can READ from (flag JackPortIsOutput),
    // audio type only, excluding our own clients. Physical (device) ports
    // come first and virtual ones (Easy Effects, OBS, monitor_*, net/
    // jacktrip...) after them; nothing is hidden, so a saved route never
    // turns into "(not found)" just because a soundcard got plugged in.
    // Each group is sorted by name.
    std::vector<JackPortInfo> inputCandidates();
    // Candidates for OUTPUT: ports we can WRITE to (flag JackPortIsInput),
    // same ordering rule: physical first, then virtual, nothing hidden.
    std::vector<JackPortInfo> outputCandidates();

    // "" disconnects. Always disconnects what was there before connecting.
    bool setInput(const std::string& port);
    // Disconnects first, then connects:
    //   l == ""            -> everything disconnected, r is ignored (a right
    //                         channel alone is not a pair)
    //   r == "" or r == l  -> a single mono connection to l
    //   otherwise          -> both l and r
    bool setOutputs(const std::string& l, const std::string& r);
    void disconnectAll();

    struct IoState
    {
        bool standalone = false;
        std::string client;                 // name of the DPF client
        std::string input;                  // port currently connected to audio_in_1 ("" if none)
        std::string outputL, outputR;       // ports currently connected from audio_out_1
        int sampleRate = 0;
        int bufferSize = 0;
        bool serverRunning = false;         // a reachable JACK server exists
    };
    IoState queryState();

private:
    struct Impl;
    Impl* impl = nullptr;
};

// Configuration persisted in ~/.config/ampforge/audio_io.txt
struct AudioIoConfig
{
    std::string input, outputL, outputR;
    bool isEmpty() const;    // all three empty
};

AudioIoConfig loadAudioIoConfig();

// Writes the config to ~/.config/ampforge/audio_io.txt.
// Returns false when the file could not be opened or the write/close failed
// (read-only HOME, full disk, path taken by a directory...), so the caller
// can report the failure instead of showing a routing that will be gone on
// the next start. The previous file is left untouched on failure only as far
// as the OS guarantees it; nothing here is atomic.
bool saveAudioIoConfig(const AudioIoConfig& config);

} // namespace ampforge

#endif // AMPFORGE_JACKIO_HPP_INCLUDED
