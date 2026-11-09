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

    // Detects "we are running as the standalone": looks for a JACK client
    // exposing the "audio_in_1" port that DPF's standalone client
    // registers. Inside a DAW our code lives in the host's process, which
    // has no client with those ports -> false.
    bool isStandalone();
    std::string pluginClient();     // "AmpForge" (or "AmpForge-01"...); "" if not standalone

    // Candidates for INPUT: ports we can READ from (flag JackPortIsOutput),
    // audio type only, excluding our own clients. If there are physical
    // ports, returns ONLY those; otherwise returns all of them. Sorted by
    // name.
    std::vector<JackPortInfo> inputCandidates();
    // Candidates for OUTPUT: ports we can WRITE to (flag JackPortIsInput),
    // same filtering/sorting.
    std::vector<JackPortInfo> outputCandidates();

    // "" disconnects. Always disconnects what was there before connecting.
    bool setInput(const std::string& port);
    // Empty r (or r == l) => a single connection. Disconnects first.
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
void saveAudioIoConfig(const AudioIoConfig& config);

} // namespace ampforge

#endif // AMPFORGE_JACKIO_HPP_INCLUDED
