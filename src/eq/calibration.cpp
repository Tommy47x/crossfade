#include "calibration.h"

#include "app_state.h"
#include "eq_server.h"
#include "mpvplayer.h"
#include "party_net.h"
#include "paths.h"
#include "ui.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef P100_WINDOWS
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace roomeq
{
namespace
{
using Clock = std::chrono::steady_clock;

enum class Mode
{
    None,
    Phone,
    PcMic,
};

double now_sec()
{
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

dsp::SweepSpec default_spec()
{
    dsp::SweepSpec s;
    s.sample_rate = 48000;
    s.f1 = 30.0;
    s.f2 = 17000.0;
    s.seconds = 6.0;
    s.lead_in = 0.8;
    s.tail = 1.2;
    s.amplitude = 0.30f;
    return s;
}

dsp::Grid default_grid()
{
    dsp::Grid g;
    g.f_lo = 20.0;
    g.f_hi = 20000.0;
    g.points = 480;
    g.build();
    return g;
}

// ---- session state (main thread) -------------------------------------------
Mode g_mode = Mode::None;
Stage g_stage = Stage::Idle;
std::string g_message;
std::string g_url;
bool g_secure = false;

dsp::SweepSpec g_spec = default_spec();
dsp::Grid g_grid = default_grid();
std::string g_mic;
dsp::TargetKind g_target_kind = dsp::TargetKind::Flat;
std::vector<dsp::CalPoint> g_cal;

std::unique_ptr<MpvPlayer> g_probe;
std::string g_sweep_path;
double g_play_at = 0.0;
double g_measure_until = 0.0;
double g_upload_deadline = 0.0;
double g_server_stop_at = 0.0;
bool g_probe_playing = false;
bool g_paused_main = false;

bool g_has_result = false;
dsp::Measurement g_meas;
std::vector<double> g_smooth;
std::vector<double> g_target;
std::vector<double> g_corrected;
dsp::Design g_design;
double g_before_db = 0.0;
int g_session = 0;

#ifndef P100_WINDOWS
pid_t g_capture_pid = -1;
#endif
std::string g_capture_path;

void (*g_wake)() = nullptr;

// ---- analysis job ----------------------------------------------------------
struct JobResult
{
    int session = 0;
    bool ok = false;
    std::string error;
    dsp::Measurement meas;
    std::vector<double> smooth;
    std::vector<double> target;
    std::vector<double> corrected;
    dsp::Design design;
    double before_db = 0.0;
};

std::mutex g_job_mutex;
JobResult g_job_result;
std::atomic<bool> g_job_done{false};
std::atomic<bool> g_job_running{false};

std::string room_eq_dir()
{
    return p100_config_path("room_eq");
}

void ensure_dir()
{
    std::error_code ec;
    std::filesystem::create_directories(room_eq_dir(), ec);
}

std::string token()
{
    static const char *alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    std::random_device rd;
    std::string out;
    for (int i = 0; i < 16; ++i)
        out += alphabet[rd() % 62];
    return out;
}

std::string timestamp_label()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%d %b %H:%M", &tm);
    return std::string("Measured ") + buf;
}

std::vector<dsp::CalPoint> load_cal(const std::string &mic, std::string &error)
{
    error.clear();
    if (mic != "custom")
        return dsp::builtin_mic_cal(mic);
    std::ifstream f(cal_file_path());
    if (!f)
    {
        error = "The imported calibration file is missing.";
        return {};
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::vector<dsp::CalPoint> pts;
    dsp::parse_cal_text(ss.str(), pts, error);
    return pts;
}

dsp::DesignLimits limits_for(const std::string &mic)
{
    dsp::DesignLimits l;
    const bool phone = (mic == "galaxy_s24" || mic == "phone_generic");
    // A phone below ~90 Hz is guesswork even with the built-in profile.
    l.lo_hz = phone ? 90.0 : 40.0;
    l.hi_hz = 16000.0;
    l.max_bands = 12;
    l.max_boost_db = 6.0;
    l.max_cut_db = -12.0;
    return l;
}

#ifndef P100_WINDOWS
bool have_exe(const char *name)
{
    const char *paths[] = {"/usr/bin/", "/bin/", "/usr/local/bin/"};
    for (const char *p : paths)
        if (access((std::string(p) + name).c_str(), X_OK) == 0)
            return true;
    return false;
}

bool spawn_capture(const std::string &device, const std::string &path, std::string &error)
{
    ensure_dir();
    const pid_t pid = fork();
    if (pid < 0)
    {
        error = "Could not start the recorder.";
        return false;
    }
    if (pid == 0)
    {
        const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0)
        {
            dup2(fd, 1);
            close(fd);
        }
        std::string dev = device.empty() ? "default" : device;
        if (have_exe("parec"))
        {
            // A small latency makes parec stream continuously; with the
            // default fragment size a suspended device can yield nothing
            // until the stream is torn down.
            std::vector<std::string> args = {"parec", "--raw", "--format=s16le",
                                             "--rate=48000", "--channels=1",
                                             "--latency-msec=50"};
            if (!device.empty())
            {
                args.push_back("--device");
                args.push_back(device);
            }
            std::vector<char *> argv;
            for (std::string &a : args)
                argv.push_back(a.data());
            argv.push_back(nullptr);
            execvp("parec", argv.data());
        }
        else
        {
            execlp("arecord", "arecord", "-q", "-f", "S16_LE", "-r", "48000", "-c", "1",
                   "-t", "raw", "-D", dev.c_str(), (char *)nullptr);
        }
        _exit(127);
    }
    g_capture_pid = pid;
    return true;
}

void stop_capture()
{
    if (g_capture_pid <= 0)
        return;
    kill(g_capture_pid, SIGTERM);
    for (int i = 0; i < 30; ++i)
    {
        int status = 0;
        if (waitpid(g_capture_pid, &status, WNOHANG) == g_capture_pid)
        {
            g_capture_pid = -1;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    kill(g_capture_pid, SIGKILL);
    waitpid(g_capture_pid, nullptr, 0);
    g_capture_pid = -1;
}

bool load_capture(std::vector<float> &samples, int &rate)
{
    std::ifstream f(g_capture_path, std::ios::binary | std::ios::ate);
    if (!f)
        return false;
    const std::streamsize bytes = f.tellg();
    if (bytes < 4800)
        return false;
    f.seekg(0);
    std::vector<std::int16_t> pcm((std::size_t)bytes / 2);
    f.read((char *)pcm.data(), (std::streamsize)pcm.size() * 2);
    samples.resize(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i)
        samples[i] = (float)pcm[i] / 32768.0f;
    rate = 48000;
    return true;
}
#endif

void ensure_probe(const AppState &state)
{
    if (!g_probe)
        g_probe = std::make_unique<MpvPlayer>(state.settings.ytdl_browser);
}

bool ensure_sweep_wav(std::string &error)
{
    ensure_dir();
    g_sweep_path = room_eq_dir() + "/sweep.wav";
    dsp::Sweep sweep = dsp::make_sweep(g_spec);
    return dsp::write_wav16(g_sweep_path, sweep.audio, g_spec.sample_rate, error);
}

void start_job(std::vector<float> samples, int rate)
{
    if (g_job_running.exchange(true))
        return;
    g_stage = Stage::Analyzing;
    g_message = "Analysing the room…";
    eqserver::set_status("analyzing", g_message);
    g_upload_deadline = 0.0;

    const int session = g_session;
    const dsp::SweepSpec spec = g_spec;
    const dsp::Grid grid = g_grid;
    const dsp::TargetKind kind = g_target_kind;
    const std::vector<dsp::CalPoint> cal = g_cal;
    const dsp::DesignLimits limits = limits_for(g_mic);

    std::thread worker([samples = std::move(samples), rate, session, spec, grid, kind, cal,
                        limits]() mutable {
        JobResult r;
        r.session = session;
        r.meas = dsp::measure(samples, rate, spec, grid);
        if (r.meas.ok)
        {
            const std::vector<double> correction = dsp::cal_curve(grid, cal);
            for (std::size_t i = 0; i < r.meas.db.size(); ++i)
                r.meas.db[i] -= correction[i];
            r.smooth = dsp::smooth_octave(grid, r.meas.db, 1.0 / 6.0);
            r.target = dsp::target_curve(grid, kind);
            r.design = dsp::design(grid, r.smooth, r.target, limits, spec.sample_rate);
            r.corrected = r.design.corrected_db;

            double sse = 0.0;
            int n = 0;
            for (std::size_t i = 0; i < grid.hz.size(); ++i)
                if (grid.hz[i] >= limits.lo_hz && grid.hz[i] <= limits.hi_hz)
                {
                    const double e = r.target[i] - r.smooth[i];
                    sse += e * e;
                    ++n;
                }
            r.before_db = n ? std::sqrt(sse / n) : 0.0;
            r.ok = true;
        }
        else
        {
            r.error = r.meas.error;
        }

        {
            std::lock_guard<std::mutex> lock(g_job_mutex);
            g_job_result = std::move(r);
        }
        g_job_running = false;
        g_job_done = true;
        if (g_wake)
            g_wake();
    });
    worker.detach();
}

void finish_job()
{
    JobResult r;
    {
        std::lock_guard<std::mutex> lock(g_job_mutex);
        r = g_job_result;
    }
    if (r.session != g_session)
        return;
    if (!r.ok)
    {
        g_stage = Stage::Error;
        g_message = r.error.empty() ? "The measurement could not be used." : r.error;
        eqserver::set_status("error", g_message);
        // Let the phone poll the error, then close the page.
        g_server_stop_at = now_sec() + 20.0;
        return;
    }
    g_meas = r.meas;
    g_smooth = r.smooth;
    g_target = r.target;
    g_corrected = r.corrected;
    g_design = r.design;
    g_before_db = r.before_db;
    g_has_result = true;
    g_stage = Stage::Ready;
    g_message = "Correction ready.";
    eqserver::set_status("done", "Done");
    // Keep the page alive briefly so the phone sees the final state.
    g_server_stop_at = now_sec() + 20.0;
}

bool take_and_start_job()
{
    std::vector<std::int16_t> pcm;
    int rate = 0;
    if (!eqserver::take_upload(pcm, rate) || pcm.size() < 2048)
        return false;
    std::vector<float> samples(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i)
        samples[i] = (float)pcm[i] / 32768.0f;
    start_job(std::move(samples), rate);
    return true;
}

void arm_playback()
{
    g_play_at = now_sec() + 0.35;
    g_message = "Starting the sweep…";
}

void play_probe(AppState &state, const MpvPlayer &player)
{
    ensure_probe(state);
    if (!g_probe)
        return;
    // Pause whatever is playing first: the sweep must be the only sound.
    if (state.playing.load() && !state.user_pause_requested && !g_paused_main)
    {
        state.user_pause_requested = true;
        g_paused_main = true;
        g_play_at = now_sec() + 0.30;
        return;
    }
    double vol = player.cached_volume();
    if (vol < 1.0)
        vol = 80.0;
    g_probe->set_volume(vol);
    g_probe->set_af("");
    g_probe->play(g_sweep_path);
    g_probe_playing = true;
    g_stage = Stage::Measuring;
    g_measure_until = now_sec() + g_spec.total_seconds() + 0.9;
    g_message = "Playing the sweep — keep quiet…";
    eqserver::set_status("measuring", "Sweep playing…");
}

void end_of_measure(AppState &state)
{
    if (g_probe && g_probe_playing)
        g_probe->stop();
    g_probe_playing = false;
    if (g_paused_main)
    {
        state.user_play_requested = true;
        g_paused_main = false;
    }
#ifndef P100_WINDOWS
    if (g_mode == Mode::PcMic)
    {
        stop_capture();
        std::vector<float> samples;
        int rate = 0;
        if (!load_capture(samples, rate))
        {
            g_stage = Stage::Error;
            g_message = "The microphone did not record anything. Check the input device.";
            eqserver::set_status("error", g_message);
            return;
        }
        start_job(std::move(samples), rate);
        return;
    }
#endif
    g_stage = Stage::Waiting;
    g_message = "Waiting for the recording…";
    g_upload_deadline = now_sec() + 180.0;
    eqserver::set_status("analyzing", "Sweep done. Waiting for the recording…");
}
} // namespace

// ---------------------------------------------------------------------------

void init()
{
    ensure_dir();
}

void set_wake(void (*fn)())
{
    g_wake = fn;
    eqserver::set_wake(fn);
}

void tick(AppState &state, const MpvPlayer &player)
{
    const double now = now_sec();
    if (g_probe)
        g_probe->process_events();
    if (g_server_stop_at > 0.0 && now >= g_server_stop_at)
    {
        if (eqserver::running())
            eqserver::stop();
        g_server_stop_at = 0.0;
    }
    if (g_stage == Stage::Analyzing)
    {
        if (g_job_done.exchange(false))
            finish_job();
        return;
    }
    // "Measure again" on the phone: accept a fresh /ready while the previous
    // result is still on screen and start over without a rescan.
    if (g_mode == Mode::Phone && (g_stage == Stage::Ready || g_stage == Stage::Error) &&
        eqserver::running() && eqserver::consume_ready())
    {
        reset_result();
        g_server_stop_at = 0.0;
        g_stage = Stage::Waiting;
        arm_playback();
    }
    if (g_stage != Stage::Waiting && g_stage != Stage::Measuring)
        return;

    if (g_stage == Stage::Waiting)
    {
        if (g_mode == Mode::Phone && eqserver::consume_ready())
            arm_playback();
        if (g_play_at > 0.0 && now >= g_play_at && !g_probe_playing)
        {
            g_play_at = 0.0;
            play_probe(state, player);
        }
        if (g_stage == Stage::Waiting && g_mode == Mode::Phone && g_upload_deadline > 0.0 &&
            now > g_upload_deadline && !g_job_running)
        {
            g_stage = Stage::Error;
            g_message = "No recording arrived from the phone. Scan the code and try again.";
            eqserver::set_status("error", g_message);
            eqserver::stop();
            return;
        }
        if (g_mode == Mode::Phone && take_and_start_job())
            return;
    }

    if (g_stage == Stage::Measuring && now >= g_measure_until)
        end_of_measure(state);
}

bool start_phone(AppState &state, std::string &error)
{
    cancel();
    g_session++;
    g_job_done = false;
    g_spec = default_spec();
    g_grid = default_grid();
    g_mic = state.settings.room_eq_mic;
    g_target_kind = state.settings.room_eq_target == "warm" ? dsp::TargetKind::Warm
                                                           : dsp::TargetKind::Flat;
    g_cal = load_cal(g_mic, error);
    if (!error.empty())
        return false;
    if (!ensure_sweep_wav(error))
        return false;

    const std::string ip = party_net::best_ip();
    if (ip.empty())
    {
        error = "No network connection: the phone cannot reach this computer.";
        return false;
    }

    eqserver::Config cfg;
    cfg.bind_ip = ip;
    cfg.port = 8585;
    cfg.token = token();
    cfg.title = "crossfade · room calibration";
    cfg.total_seconds = g_spec.total_seconds();
    cfg.sample_rate = g_spec.sample_rate;
    if (!eqserver::start(cfg, error))
        return false;
    eqserver::set_wake(g_wake);

    g_url = eqserver::url();
    g_secure = eqserver::secure();
    g_mode = Mode::Phone;
    g_stage = Stage::Waiting;
    g_message = "Scan the QR code with your phone.";
    return true;
}

bool start_pc_mic(AppState &state, const std::string &device, std::string &error)
{
    cancel();
#ifndef P100_WINDOWS
    g_session++;
    g_job_done = false;
    g_spec = default_spec();
    g_grid = default_grid();
    g_mic = state.settings.room_eq_mic;
    g_target_kind = state.settings.room_eq_target == "warm" ? dsp::TargetKind::Warm
                                                           : dsp::TargetKind::Flat;
    g_cal = load_cal(g_mic, error);
    if (!error.empty())
        return false;
    if (!ensure_sweep_wav(error))
        return false;
    g_capture_path = room_eq_dir() + "/capture.raw";
    if (!spawn_capture(device, g_capture_path, error))
        return false;
    g_mode = Mode::PcMic;
    g_stage = Stage::Waiting;
    g_message = "Recording with the computer microphone…";
    arm_playback();
    return true;
#else
    (void)state;
    (void)device;
    error = "Computer-microphone capture is not available on Windows yet. "
            "Use the phone page or import a recording.";
    return false;
#endif
}

bool play_sweep(AppState &state, std::string &error)
{
    if (g_stage == Stage::Idle || g_mode != Mode::Phone)
    {
        if (!start_phone(state, error))
            return false;
    }
    if (g_stage == Stage::Measuring || g_probe_playing)
    {
        error = "The sweep is already playing.";
        return false;
    }
    if (!ensure_sweep_wav(error))
        return false;
    g_stage = Stage::Waiting;
    arm_playback();
    return true;
}

void cancel()
{
    g_session++;
    g_play_at = 0.0;
    g_measure_until = 0.0;
    g_upload_deadline = 0.0;
    g_server_stop_at = 0.0;
    if (g_probe && g_probe_playing)
        g_probe->stop();
    g_probe_playing = false;
    g_paused_main = false;
#ifndef P100_WINDOWS
    stop_capture();
#endif
    if (eqserver::running())
        eqserver::stop();
    g_mode = Mode::None;
    g_stage = Stage::Idle;
    g_message.clear();
    g_url.clear();
    g_job_done = false;
}

Status status()
{
    Status s;
    s.stage = g_stage;
    s.message = g_message;
    s.url = g_url;
    s.secure = g_secure;
    s.pc_mic = (g_mode == Mode::PcMic);
    s.has_result = g_has_result;
    s.sample_rate = g_spec.sample_rate;
    s.residual_before_db = g_before_db;
    s.residual_after_db = g_design.residual_rms_db;
    if (g_has_result)
    {
        s.hz = g_grid.hz;
        s.measured_db = g_smooth;
        s.target_db = g_target;
        s.corrected_db = g_corrected;
        s.bands = g_design.bands;
    }
    return s;
}

bool apply(AppState &state)
{
    if (!g_has_result)
        return false;
    Settings &st = state.settings;
    st.room_eq_bands.clear();
    for (const dsp::Band &b : g_design.bands)
        st.room_eq_bands.push_back({(float)b.freq, (float)b.gain, (float)b.q});
    st.room_eq_preamp = (float)g_design.preamp_db;
    st.room_eq_residual_db = (float)g_design.residual_rms_db;
    st.room_eq_label = timestamp_label();
    st.room_eq_enabled = !st.room_eq_bands.empty();
    save_settings(st);
    return true;
}

void forget(AppState &state)
{
    reset_result();
    Settings &st = state.settings;
    st.room_eq_bands.clear();
    st.room_eq_enabled = false;
    st.room_eq_preamp = 0.0f;
    st.room_eq_label.clear();
    st.room_eq_residual_db = 0.0f;
    save_settings(st);
}

void reset_result()
{
    g_has_result = false;
    g_design = dsp::Design{};
    g_meas = dsp::Measurement{};
    g_smooth.clear();
    g_target.clear();
    g_corrected.clear();
    g_before_db = 0.0;
}

bool import_cal(AppState &state, const std::string &path, std::string &error)
{
    std::ifstream f(path);
    if (!f)
    {
        error = "Could not open that file.";
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::vector<dsp::CalPoint> pts;
    if (!dsp::parse_cal_text(ss.str(), pts, error))
        return false;
    ensure_dir();
    std::ofstream out(cal_file_path());
    if (!out)
    {
        error = "Could not save the calibration file.";
        return false;
    }
    out << ss.str();
    state.settings.room_eq_mic = "custom";
    save_settings(state.settings);
    return true;
}

std::string cal_file_path()
{
    return room_eq_dir() + "/mic.cal";
}

std::vector<std::string> input_devices()
{
    std::vector<std::string> out;
    out.push_back(""); // system default
#ifndef P100_WINDOWS
    FILE *p = popen("pactl list short sources 2>/dev/null", "r");
    if (p)
    {
        char line[512];
        while (std::fgets(line, sizeof(line), p))
        {
            std::istringstream ss(line);
            std::string index, name;
            ss >> index >> name;
            if (name.empty() || name.find(".monitor") != std::string::npos)
                continue;
            if (std::find(out.begin(), out.end(), name) == out.end())
                out.push_back(name);
        }
        pclose(p);
    }
#endif
    return out;
}
} // namespace roomeq
