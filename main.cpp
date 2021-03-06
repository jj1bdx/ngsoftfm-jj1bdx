// NGSoftFM - Software decoder for FM broadcast radio with RTL-SDR
//
// Copyright (C) 2013, Joris van Rantwijk.
// Copyright (C) 2015 Edouard Griffiths, F4EXB
// Copyright (C) 2018 Kenji Rikitake, JJ1BDX
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, see http://www.gnu.org/licenses/gpl-2.0.html

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <memory>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

#include "AudioOutput.h"
#include "DataBuffer.h"
#include "FmDecode.h"
#include "MovingAverage.h"
#include "SoftFM.h"
#include "util.h"

#include "AirspySource.h"
#include "HackRFSource.h"
#include "RtlSdrSource.h"

#define NGSOFTFM_VERSION "0.1.14"

/** Flag is set on SIGINT / SIGTERM. */
static std::atomic_bool stop_flag(false);

/** Simple linear gain adjustment. */
void adjust_gain(SampleVector &samples, double gain) {
  for (unsigned int i = 0, n = samples.size(); i < n; i++) {
    samples[i] *= gain;
  }
}

/**
 * Get data from output buffer and write to output stream.
 *
 * This code runs in a separate thread.
 */
void write_output_data(AudioOutput *output, DataBuffer<Sample> *buf,
                       unsigned int buf_minfill) {
  while (!stop_flag.load()) {

    if (buf->queued_samples() == 0) {
      // The buffer is empty. Perhaps the output stream is consuming
      // samples faster than we can produce them. Wait until the buffer
      // is back at its nominal level to make sure this does not happen
      // too often.
      buf->wait_buffer_fill(buf_minfill);
    }

    if (buf->pull_end_reached()) {
      // Reached end of stream.
      break;
    }

    // Get samples from buffer and write to output.
    SampleVector samples = buf->pull();
    output->write(samples);
    if (!(*output)) {
      fprintf(stderr, "ERROR: AudioOutput: %s\n", output->error().c_str());
    }
  }
}

/** Handle Ctrl-C and SIGTERM. */
static void handle_sigterm(int sig) {
  stop_flag.store(true);

  std::string msg = "\nGot signal ";
  msg += strsignal(sig);
  msg += ", stopping ...\n";

  const char *s = msg.c_str();
  ssize_t size = write(STDERR_FILENO, s, strlen(s));
  size++; // dummy
}

void usage() {
  fprintf(
      stderr,
      "Usage: ngsoftfm [options]\n"
      "  -t devtype     Device type:\n"
      "                   - rtlsdr: RTL-SDR devices\n"
      "                   - hackrf: HackRF One or Jawbreaker\n"
      "                   - airspy: Airspy\n"
      "  -c config      Comma separated key=value configuration pairs or just "
      "key for switches\n"
      "                 See below for valid values per device type\n"
      "  -d devidx      Device index, 'list' to show device list (default 0)\n"
      "  -r pcmrate     Audio sample rate in Hz (default 48000 Hz)\n"
      "  -M             Disable stereo decoding\n"
      "  -R filename    Write audio data as raw S16_LE samples\n"
      "                 use filename '-' to write to stdout\n"
      "  -W filename    Write audio data to .WAV file\n"
      "  -P [device]    Play audio via ALSA device (default 'default')\n"
      "  -T filename    Write pulse-per-second timestamps\n"
      "                 use filename '-' to write to stdout\n"
      "  -b seconds     Set audio buffer size in seconds\n"
      "  -X             Shift pilot phase (for Quadrature Multipath Monitor)\n"
      "                 (-X is ignored under mono mode (-M))\n"
      "  -U             Set deemphasis to 75 microseconds (default: 50)\n"
      "\n"
      "Configuration options for RTL-SDR devices\n"
      "  freq=<int>     Frequency of radio station in Hz (default 100000000)\n"
      "                 valid values: 10M to 2.2G (working range depends on "
      "device)\n"
      "  srate=<int>    IF sample rate in Hz (default 960000)\n"
      "                 (valid ranges: [225001, 300000], [900001, 3200000]))\n"
      "  gain=<float>   Set LNA gain in dB, or 'auto',\n"
      "                 or 'list' to just get a list of valid values (default "
      "auto)\n"
      "  blklen=<int>   Set audio buffer size in seconds (default RTL-SDR "
      "default)\n"
      "  agc            Enable RTL AGC mode (default disabled)\n"
      "\n"
      "Configuration options for HackRF devices\n"
      "  freq=<int>     Frequency of radio station in Hz (default 100000000)\n"
      "                 valid values: 1M to 6G\n"
      "  srate=<int>    IF sample rate in Hz (default 5000000)\n"
      "                 (valid ranges: [2500000,20000000]))\n"
      "  lgain=<int>    LNA gain in dB. 'list' to just get a list of valid "
      "values: (default 16)\n"
      "  vgain=<int>    VGA gain in dB. 'list' to just get a list of valid "
      "values: (default 22)\n"
      "  bwfilter=<int> Filter bandwidth in MHz. 'list' to just get a list of "
      "valid values: (default 2.5)\n"
      "  extamp         Enable extra RF amplifier (default disabled)\n"
      "  antbias        Enable antemma bias (default disabled)\n"
      "\n"
      "Configuration options for Airspy devices\n"
      "  freq=<int>     Frequency of radio station in Hz (default 100000000)\n"
      "                 valid values: 24M to 1.8G\n"
      "  srate=<int>    IF sample rate in Hz. Depends on Airspy firmware and "
      "libairspy support\n"
      "                 Airspy firmware and library must support dynamic "
      "sample rate query. (default 10000000)\n"
      "  lgain=<int>    LNA gain in dB. 'list' to just get a list of valid "
      "values: (default 8)\n"
      "  mgain=<int>    Mixer gain in dB. 'list' to just get a list of valid "
      "values: (default 8)\n"
      "  vgain=<int>    VGA gain in dB. 'list' to just get a list of valid "
      "values: (default 8)\n"
      "  antbias        Enable antemma bias (default disabled)\n"
      "  lagc           Enable LNA AGC (default disabled)\n"
      "  magc           Enable mixer AGC (default disabled)\n"
      "\n");
}

void badarg(const char *label) {
  usage();
  fprintf(stderr, "ERROR: Invalid argument for %s\n", label);
  exit(1);
}

bool parse_int(const char *s, int &v, bool allow_unit = false) {
  char *endp;
  long t = strtol(s, &endp, 10);
  if (endp == s) {
    return false;
  }
  if (allow_unit && *endp == 'k' && t > INT_MIN / 1000 && t < INT_MAX / 1000) {
    t *= 1000;
    endp++;
  }
  if (*endp != '\0' || t < INT_MIN || t > INT_MAX) {
    return false;
  }
  v = t;
  return true;
}

/** Return Unix time stamp in seconds. */
double get_time() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec + 1.0e-6 * tv.tv_usec;
}

static bool get_device(std::vector<std::string> &devnames, std::string &devtype,
                       Source **srcsdr, int devidx) {
  if (strcasecmp(devtype.c_str(), "rtlsdr") == 0) {
    RtlSdrSource::get_device_names(devnames);
  } else if (strcasecmp(devtype.c_str(), "hackrf") == 0) {
    HackRFSource::get_device_names(devnames);
  } else if (strcasecmp(devtype.c_str(), "airspy") == 0) {
    AirspySource::get_device_names(devnames);
  } else {
    fprintf(
        stderr,
        "ERROR: wrong device type (-t option) must be one of the following:\n");
    fprintf(stderr, "       rtlsdr, hackrf, airspy\n");
    return false;
  }

  if (devidx < 0 || (unsigned int)devidx >= devnames.size()) {
    if (devidx != -1) {
      fprintf(stderr, "ERROR: invalid device index %d\n", devidx);
    }

    fprintf(stderr, "Found %u devices:\n", (unsigned int)devnames.size());

    for (unsigned int i = 0; i < devnames.size(); i++) {
      fprintf(stderr, "%2u: %s\n", i, devnames[i].c_str());
    }

    return false;
  }

  fprintf(stderr, "using device %d: %s\n", devidx, devnames[devidx].c_str());

  if (strcasecmp(devtype.c_str(), "rtlsdr") == 0) {
    // Open RTL-SDR device.
    *srcsdr = new RtlSdrSource(devidx);
  } else if (strcasecmp(devtype.c_str(), "hackrf") == 0) {
    // Open HackRF device.
    *srcsdr = new HackRFSource(devidx);
  } else if (strcasecmp(devtype.c_str(), "airspy") == 0) {
    // Open Airspy device.
    *srcsdr = new AirspySource(devidx);
  }

  return true;
}

// static constants in FmDecoder must be declared here
// See
// https://gcc.gnu.org/wiki/VerboseDiagnostics#missing_static_const_definition

constexpr double FmDecoder::default_deemphasis;
constexpr double FmDecoder::default_bandwidth_if;
constexpr double FmDecoder::default_freq_dev;
constexpr double FmDecoder::default_bandwidth_pcm;
constexpr double FmDecoder::pilot_freq;
constexpr unsigned int FmDecoder::finetuner_table_size;
constexpr double FmDecoder::default_deemphasis_eu;
constexpr double FmDecoder::default_deemphasis_na;

int main(int argc, char **argv) {
  int devidx = 0;
  int pcmrate = 48000;
  bool stereo = true;
  enum OutputMode { MODE_RAW, MODE_WAV, MODE_ALSA };
#ifdef USE_ALSA
  OutputMode outmode = MODE_ALSA;
  std::string filename;
#else  // !USE_ALSA
  OutputMode outmode = MODE_RAW;
  std::string filename("-");
#endif // USE_ALSA
  std::string alsadev("default");
  bool quietmode = false;
  std::string ppsfilename;
  FILE *ppsfile = NULL;
  double bufsecs = -1;
  bool pilot_shift = false;
  bool deemphasis_na = false;
  std::string config_str;
  std::string devtype_str;
  std::vector<std::string> devnames;
  Source *srcsdr = 0;

  fprintf(stderr, "NGSoftFM - Software decoder for FM broadcast radio\n");
  fprintf(stderr, "ngsoftfm-jj1bdx " NGSOFTFM_VERSION "\n");

  const struct option longopts[] = {
      {"devtype", 2, NULL, 't'}, {"config", 2, NULL, 'c'},
      {"dev", 1, NULL, 'd'},     {"pcmrate", 1, NULL, 'r'},
      {"mono", 0, NULL, 'M'},    {"raw", 1, NULL, 'R'},
      {"wav", 1, NULL, 'W'},     {"play", 2, NULL, 'P'},
      {"pps", 1, NULL, 'T'},     {"buffer", 1, NULL, 'b'},
      {"quiet", 1, NULL, 'q'},   {"pilotshift", 0, NULL, 'X'},
      {"usa", 0, NULL, 'U'},     {NULL, 0, NULL, 0}};

  int c, longindex;
  while ((c = getopt_long(argc, argv, "t:c:d:r:MR:W:P::T:b:qXU", longopts,
                          &longindex)) >= 0) {
    switch (c) {
    case 't':
      devtype_str.assign(optarg);
      break;
    case 'c':
      config_str.assign(optarg);
      break;
    case 'd':
      if (!parse_int(optarg, devidx)) {
        devidx = -1;
      }
      break;
    case 'r':
      if (!parse_int(optarg, pcmrate, true) || pcmrate < 1) {
        badarg("-r");
      }
      break;
    case 'M':
      stereo = false;
      break;
    case 'R':
      outmode = MODE_RAW;
      filename = optarg;
      break;
    case 'W':
      outmode = MODE_WAV;
      filename = optarg;
      break;
    case 'P':
      outmode = MODE_ALSA;
      if (optarg != NULL) {
        alsadev = optarg;
      }
      break;
    case 'T':
      ppsfilename = optarg;
      break;
    case 'b':
      if (!parse_dbl(optarg, bufsecs) || bufsecs < 0) {
        badarg("-b");
      }
      break;
    case 'q':
      quietmode = true;
      break;
    case 'X':
      pilot_shift = true;
      break;
    case 'U':
      deemphasis_na = true;
      break;
    default:
      usage();
      fprintf(stderr, "ERROR: Invalid command line options\n");
      exit(1);
    }
  }

  if (optind < argc) {
    usage();
    fprintf(stderr, "ERROR: Unexpected command line options\n");
    exit(1);
  }

  // Catch Ctrl-C and SIGTERM
  struct sigaction sigact;
  sigact.sa_handler = handle_sigterm;
  sigemptyset(&sigact.sa_mask);
  sigact.sa_flags = SA_RESETHAND;

  if (sigaction(SIGINT, &sigact, NULL) < 0) {
    fprintf(stderr, "WARNING: can not install SIGINT handler (%s)\n",
            strerror(errno));
  }

  if (sigaction(SIGTERM, &sigact, NULL) < 0) {
    fprintf(stderr, "WARNING: can not install SIGTERM handler (%s)\n",
            strerror(errno));
  }

  // Open PPS file.
  if (!ppsfilename.empty()) {
    if (ppsfilename == "-") {
      fprintf(stderr, "writing pulse-per-second markers to stdout\n");
      ppsfile = stdout;
    } else {
      fprintf(stderr, "writing pulse-per-second markers to '%s'\n",
              ppsfilename.c_str());
      ppsfile = fopen(ppsfilename.c_str(), "w");

      if (ppsfile == NULL) {
        fprintf(stderr, "ERROR: can not open '%s' (%s)\n", ppsfilename.c_str(),
                strerror(errno));
        exit(1);
      }
    }

    fprintf(ppsfile, "#pps_index sample_index   unix_time\n");
    fflush(ppsfile);
  }

  // Calculate number of samples in audio buffer.
  unsigned int outputbuf_samples = 0;

  if (bufsecs < 0 &&
      (outmode == MODE_ALSA || (outmode == MODE_RAW && filename == "-"))) {
    // Set default buffer to 1 second for interactive output streams.
    outputbuf_samples = pcmrate;
  } else if (bufsecs > 0) {
    // Calculate nr of samples for configured buffer length.
    outputbuf_samples = (unsigned int)(bufsecs * pcmrate);
  }

  if (outputbuf_samples > 0) {
    fprintf(stderr, "output buffer:     %.1f seconds\n",
            outputbuf_samples / double(pcmrate));
  }

  // Prepare output writer.
  std::unique_ptr<AudioOutput> audio_output;

  switch (outmode) {
  case MODE_RAW:
    fprintf(stderr, "writing raw 16-bit audio samples to '%s'\n",
            filename.c_str());
    audio_output.reset(new RawAudioOutput(filename));
    break;
  case MODE_WAV:
    fprintf(stderr, "writing audio samples to '%s'\n", filename.c_str());
    audio_output.reset(new WavAudioOutput(filename, pcmrate, stereo));
    break;
  case MODE_ALSA:
#ifdef USE_ALSA
    fprintf(stderr, "playing audio to ALSA device '%s'\n", alsadev.c_str());
    audio_output.reset(new AlsaAudioOutput(alsadev, pcmrate, stereo));
    break;
#else  // !USE_ALSA
    fprintf(stderr, "ALSA not implemented\n");
    exit(1);
#endif // USE_ALSA
  }

  if (!(*audio_output)) {
    fprintf(stderr, "ERROR: AudioOutput: %s\n", audio_output->error().c_str());
    exit(1);
  }

  if (!get_device(devnames, devtype_str, &srcsdr, devidx)) {
    exit(1);
  }

  if (!(*srcsdr)) {
    fprintf(stderr, "ERROR source: %s\n", srcsdr->error().c_str());
    delete srcsdr;
    exit(1);
  }

  // Configure device and start streaming.
  if (!srcsdr->configure(config_str)) {
    fprintf(stderr, "ERROR: configuration: %s\n", srcsdr->error().c_str());
    delete srcsdr;
    exit(1);
  }

  double freq = srcsdr->get_configured_frequency();
  fprintf(stderr, "tuned for:         %.6f MHz\n", freq * 1.0e-6);

  double tuner_freq = srcsdr->get_frequency();
  fprintf(stderr, "device tuned for:  %.6f MHz\n", tuner_freq * 1.0e-6);

  double ifrate = srcsdr->get_sample_rate();
  fprintf(stderr, "IF sample rate:    %.0f Hz\n", ifrate);

  double delta_if = tuner_freq - freq;
  MovingAverage<float> ppm_average(40, 0.0f);

  srcsdr->print_specific_parms();

  // Create source data queue.
  DataBuffer<IQSample> source_buffer;

  // ownership will be transferred to thread therefore the unique_ptr with move
  // is convenient if the pointer is to be shared with the main thread use
  // shared_ptr (and no move) instead
  std::unique_ptr<Source> up_srcsdr(srcsdr);

  // Start reading from device in separate thread.
  // std::thread source_thread(read_source_data, std::move(up_srcsdr),
  // &source_buffer);
  up_srcsdr->start(&source_buffer, &stop_flag);

  if (!up_srcsdr) {
    fprintf(stderr, "ERROR: source: %s\n", up_srcsdr->error().c_str());
    exit(1);
  }

  // We can downsample to the (default_bandwidth_if * 2) * 1.1
  // without loss of information.
  // This will speed up later processing stages.
  double downsample_target = FmDecoder::default_bandwidth_if * 2.2;
  unsigned int downsample = std::max(1, int(ifrate / downsample_target));

  // Prevent aliasing at very low output sample rates.
  double bandwidth_pcm =
      std::min(FmDecoder::default_bandwidth_pcm, 0.45 * pcmrate);
  double deemphasis = deemphasis_na ? FmDecoder::default_deemphasis_na
                                    : FmDecoder::default_deemphasis_eu;

  fprintf(stderr, "if -> baseband:    %u (downsampled by)\n", downsample);
  fprintf(stderr, "audio sample rate: %u Hz\n", pcmrate);
  fprintf(stderr, "audio bandwidth:   %.3f kHz\n", bandwidth_pcm * 1.0e-3);
  fprintf(stderr, "deemphasis:        %.1f microseconds\n", deemphasis);

  // Prepare decoder.
  FmDecoder fm(ifrate,                          // sample_rate_if
               freq - tuner_freq,               // tuning_offset
               pcmrate,                         // sample_rate_pcm
               stereo,                          // stereo
               deemphasis,                      // deemphasis,
               FmDecoder::default_bandwidth_if, // bandwidth_if
               FmDecoder::default_freq_dev,     // freq_dev
               bandwidth_pcm,                   // bandwidth_pcm
               downsample,                      // downsample
               pilot_shift);                    // pilot_shift

  // If buffering enabled, start background output thread.
  DataBuffer<Sample> output_buffer;
  std::thread output_thread;

  if (outputbuf_samples > 0) {
    unsigned int nchannel = stereo ? 2 : 1;
    output_thread = std::thread(write_output_data, audio_output.get(),
                                &output_buffer, outputbuf_samples * nchannel);
  }

  SampleVector audiosamples;
  bool inbuf_length_warning = false;
  double audio_level = 0;
  bool got_stereo = false;

  double block_time = get_time();

  // Main loop.
  for (unsigned int block = 0; !stop_flag.load(); block++) {

    // Check for overflow of source buffer.
    if (!inbuf_length_warning && source_buffer.queued_samples() > 10 * ifrate) {
      fprintf(stderr, "\nWARNING: Input buffer is growing (system too slow)\n");
      inbuf_length_warning = true;
    }

    // Pull next block from source buffer.
    IQSampleVector iqsamples = source_buffer.pull();

    if (iqsamples.empty()) {
      break;
    }

    double prev_block_time = block_time;
    block_time = get_time();

    // Decode FM signal.
    fm.process(iqsamples, audiosamples);

    // Measure audio level.
    double audio_mean, audio_rms;
    samples_mean_rms(audiosamples, audio_mean, audio_rms);
    audio_level = 0.95 * audio_level + 0.05 * audio_rms;

    // Set nominal audio volume.
    adjust_gain(audiosamples, 0.5);

    // the minus factor is to show the ppm correction
    // to make and not the one made
    ppm_average.feed(((fm.get_tuning_offset() + delta_if) / tuner_freq) *
                     -1.0e6);
    double ppm_error = (tuner_freq + fm.get_tuning_offset()) * 1.0e-6;
    double ppm_value_average = ppm_average.average();
    double if_level_db = 20 * log10(fm.get_if_level());
    double baseband_level_db = 20 * log10(fm.get_baseband_level()) + 3.01;
    double audio_level_db = 20 * log10(audio_level) + 3.01;

    double buflen_sec;
    if (outputbuf_samples > 0) {
      unsigned int nchannel = stereo ? 2 : 1;
      std::size_t buflen = output_buffer.queued_samples();
      buflen_sec = buflen / nchannel / double(pcmrate);
    } else {
      buflen_sec = -1.0;
    }

    // Show status messages for each block if not in quiet mode.
    if (!quietmode) {
      // Show per-block statistics.
      fprintf(stderr,
              "\rblk=%6d:f=%8.4fMHz:ppm=%+6.2f:IF=%+5.1fdB:"
              "BB=%+5.1fdB:AF=%+5.1fdB:buf=%.1fs",
              block, ppm_error, ppm_value_average, if_level_db,
              baseband_level_db, audio_level_db, buflen_sec);
      // Show stereo status.
      if (fm.stereo_detected() != got_stereo) {
        got_stereo = fm.stereo_detected();
        if (got_stereo) {
          fprintf(stderr, "\ngot stereo signal (pilot level = %f)\n",
                  fm.get_pilot_level());
        } else {
          fprintf(stderr, "\nlost stereo signal\n");
        }
      }
      fflush(stderr);
    }

    // Write PPS markers.
    if (ppsfile != NULL) {
      for (const PilotPhaseLock::PpsEvent &ev : fm.get_pps_events()) {
        double ts = prev_block_time;
        ts += ev.block_position * (block_time - prev_block_time);
        fprintf(ppsfile, "%8s %14s %18.6f\n",
                std::to_string(ev.pps_index).c_str(),
                std::to_string(ev.sample_index).c_str(), ts);
        fflush(ppsfile);
      }
    }

    // Throw away first 4 blocks.
    // They are noisy because IF filters are still starting up.
    // (Increased from one to support high sampling rates)
    if (block > 4) {
      // Write samples to output.
      if (outputbuf_samples > 0) {
        // Buffered write.
        output_buffer.push(move(audiosamples));
      } else {
        // Direct write.
        audio_output->write(audiosamples);
      }
    }
  }

  fprintf(stderr, "\n");

  // Join background threads.
  // source_thread.join();
  up_srcsdr->stop();

  if (outputbuf_samples > 0) {
    output_buffer.push_end();
    output_thread.join();
  }

  // No cleanup needed; everything handled by destructors

  return 0;
}

/* end */
