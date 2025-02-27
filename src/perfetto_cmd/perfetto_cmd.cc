/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/perfetto_cmd/perfetto_cmd.h"

#include "perfetto/base/build_config.h"
#include "perfetto/ext/base/scoped_file.h"

#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

// For dup().
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <io.h>
#else
#include <unistd.h>
#endif

#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/android_utils.h"
#include "perfetto/ext/base/ctrl_c_handler.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/getopt.h"
#include "perfetto/ext/base/pipe.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/thread_utils.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/base/uuid.h"
#include "perfetto/ext/base/version.h"
#include "perfetto/ext/traced/traced.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/trace_packet.h"
#include "perfetto/ext/tracing/ipc/default_socket.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/core/trace_config.h"
#include "perfetto/tracing/core/tracing_service_state.h"
#include "src/android_stats/statsd_logging_helper.h"
#include "src/perfetto_cmd/config.h"
#include "src/perfetto_cmd/packet_writer.h"
#include "src/perfetto_cmd/pbtxt_to_pb.h"
#include "src/perfetto_cmd/trigger_producer.h"

#include "protos/perfetto/common/tracing_service_state.gen.h"

namespace perfetto {
namespace {

perfetto::PerfettoCmd* g_perfetto_cmd;

uint32_t kOnTraceDataTimeoutMs = 3000;

class LoggingErrorReporter : public ErrorReporter {
 public:
  LoggingErrorReporter(std::string file_name, const char* config)
      : file_name_(file_name), config_(config) {}

  void AddError(size_t row,
                size_t column,
                size_t length,
                const std::string& message) override {
    parsed_successfully_ = false;
    std::string line = ExtractLine(row - 1).ToStdString();
    if (!line.empty() && line[line.length() - 1] == '\n') {
      line.erase(line.length() - 1);
    }

    std::string guide(column + length, ' ');
    for (size_t i = column; i < column + length; i++) {
      guide[i - 1] = i == column ? '^' : '~';
    }
    fprintf(stderr, "%s:%zu:%zu error: %s\n", file_name_.c_str(), row, column,
            message.c_str());
    fprintf(stderr, "%s\n", line.c_str());
    fprintf(stderr, "%s\n", guide.c_str());
  }

  bool Success() const { return parsed_successfully_; }

 private:
  base::StringView ExtractLine(size_t line) {
    const char* start = config_;
    const char* end = config_;

    for (size_t i = 0; i < line + 1; i++) {
      start = end;
      char c;
      while ((c = *end++) && c != '\n')
        ;
    }
    return base::StringView(start, static_cast<size_t>(end - start));
  }

  bool parsed_successfully_ = true;
  std::string file_name_;
  const char* config_;
};

bool ParseTraceConfigPbtxt(const std::string& file_name,
                           const std::string& pbtxt,
                           TraceConfig* config) {
  LoggingErrorReporter reporter(file_name, pbtxt.c_str());
  std::vector<uint8_t> buf = PbtxtToPb(pbtxt, &reporter);
  if (!reporter.Success())
    return false;
  if (!config->ParseFromArray(buf.data(), buf.size()))
    return false;
  return true;
}

bool IsUserBuild() {
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
  std::string build_type = base::GetAndroidProp("ro.build.type");
  if (build_type.empty()) {
    PERFETTO_ELOG("Unable to read ro.build.type: assuming user build");
    return true;
  }
  return build_type == "user";
#else
  return false;
#endif  // PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
}

base::Optional<PerfettoStatsdAtom> ConvertRateLimiterResponseToAtom(
    RateLimiter::ShouldTraceResponse resp) {
  switch (resp) {
    case RateLimiter::kNotAllowedOnUserBuild:
      return PerfettoStatsdAtom::kCmdUserBuildTracingNotAllowed;
    case RateLimiter::kFailedToInitState:
      return PerfettoStatsdAtom::kCmdFailedToInitGuardrailState;
    case RateLimiter::kInvalidState:
      return PerfettoStatsdAtom::kCmdInvalidGuardrailState;
    case RateLimiter::kHitUploadLimit:
      return PerfettoStatsdAtom::kCmdHitUploadLimit;
    case RateLimiter::kOkToTrace:
      return base::nullopt;
  }
  PERFETTO_FATAL("For GCC");
}

}  // namespace

const char* kStateDir = "/data/misc/perfetto-traces";

PerfettoCmd::PerfettoCmd() {
  PERFETTO_DCHECK(!g_perfetto_cmd);
  g_perfetto_cmd = this;
}

PerfettoCmd::~PerfettoCmd() {
  PERFETTO_DCHECK(g_perfetto_cmd == this);
  g_perfetto_cmd = nullptr;
}

void PerfettoCmd::PrintUsage(const char* argv0) {
  fprintf(stderr, R"(
Usage: %s
  --background     -d      : Exits immediately and continues in the background.
                             Prints the PID of the bg process. The printed PID
                             can used to gracefully terminate the tracing
                             session by issuing a `kill -TERM $PRINTED_PID`.
  --background-wait -D     : Like --background, but waits (up to 30s) for all
                             data sources to be started before exiting. Exit
                             code is zero if a successful acknowledgement is
                             received, non-zero otherwise (error or timeout).
  --config         -c      : /path/to/trace/config/file or - for stdin
  --out            -o      : /path/to/out/trace/file or - for stdout
  --txt                    : Parse config as pbtxt. Not for production use.
                             Not a stable API.
  --query                  : Queries the service state and prints it as
                             human-readable text.
  --query-raw              : Like --query, but prints raw proto-encoded bytes
                             of tracing_service_state.proto.
  --help           -h

Light configuration flags: (only when NOT using -c/--config)
  --time           -t      : Trace duration N[s,m,h] (default: 10s)
  --buffer         -b      : Ring buffer size N[mb,gb] (default: 32mb)
  --size           -s      : Max file size N[mb,gb]
                            (default: in-memory ring-buffer only)
  --app            -a      : Android (atrace) app name
  FTRACE_GROUP/FTRACE_NAME : Record ftrace event (e.g. sched/sched_switch)
  ATRACE_CAT               : Record ATRACE_CAT (e.g. wm) (Android only)

Statsd-specific and other Android-only flags:
  --alert-id           : ID of the alert that triggered this trace.
  --config-id          : ID of the triggering config.
  --config-uid         : UID of app which registered the config.
  --subscription-id    : ID of the subscription that triggered this trace.
  --upload             : Upload trace.
  --dropbox        TAG : DEPRECATED: Use --upload instead
                         TAG should always be set to 'perfetto'.
  --save-for-bugreport : If a trace with bugreport_score > 0 is running, it
                         saves it into a file. Outputs the path when done.
  --no-guardrails      : Ignore guardrails triggered when using --upload
                         (testing only).
  --reset-guardrails   : Resets the state of the guardails and exits
                         (testing only).

Detach mode. DISCOURAGED, read https://perfetto.dev/docs/concepts/detached-mode
  --detach=key          : Detach from the tracing session with the given key.
  --attach=key [--stop] : Re-attach to the session (optionally stop tracing
                          once reattached).
  --is_detached=key     : Check if the session can be re-attached.
                          Exit code:  0:Yes, 2:No, 1:Error.
)", /* this comment fixes syntax highlighting in some editors */
          argv0);
}

base::Optional<int> PerfettoCmd::ParseCmdlineAndMaybeDaemonize(int argc,
                                                               char** argv) {
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  umask(0000);  // make sure that file creation is not affected by umask.
#endif
  enum LongOption {
    OPT_ALERT_ID = 1000,
    OPT_BUGREPORT,
    OPT_CONFIG_ID,
    OPT_CONFIG_UID,
    OPT_SUBSCRIPTION_ID,
    OPT_RESET_GUARDRAILS,
    OPT_PBTXT_CONFIG,
    OPT_DROPBOX,
    OPT_UPLOAD,
    OPT_IGNORE_GUARDRAILS,
    OPT_DETACH,
    OPT_ATTACH,
    OPT_IS_DETACHED,
    OPT_STOP,
    OPT_QUERY,
    OPT_QUERY_RAW,
    OPT_VERSION,
  };
  static const option long_options[] = {
      {"help", no_argument, nullptr, 'h'},
      {"config", required_argument, nullptr, 'c'},
      {"out", required_argument, nullptr, 'o'},
      {"background", no_argument, nullptr, 'd'},
      {"background-wait", no_argument, nullptr, 'D'},
      {"time", required_argument, nullptr, 't'},
      {"buffer", required_argument, nullptr, 'b'},
      {"size", required_argument, nullptr, 's'},
      {"app", required_argument, nullptr, 'a'},
      {"no-guardrails", no_argument, nullptr, OPT_IGNORE_GUARDRAILS},
      {"txt", no_argument, nullptr, OPT_PBTXT_CONFIG},
      {"upload", no_argument, nullptr, OPT_UPLOAD},
      {"dropbox", required_argument, nullptr, OPT_DROPBOX},
      {"alert-id", required_argument, nullptr, OPT_ALERT_ID},
      {"config-id", required_argument, nullptr, OPT_CONFIG_ID},
      {"config-uid", required_argument, nullptr, OPT_CONFIG_UID},
      {"subscription-id", required_argument, nullptr, OPT_SUBSCRIPTION_ID},
      {"reset-guardrails", no_argument, nullptr, OPT_RESET_GUARDRAILS},
      {"detach", required_argument, nullptr, OPT_DETACH},
      {"attach", required_argument, nullptr, OPT_ATTACH},
      {"is_detached", required_argument, nullptr, OPT_IS_DETACHED},
      {"stop", no_argument, nullptr, OPT_STOP},
      {"query", no_argument, nullptr, OPT_QUERY},
      {"query-raw", no_argument, nullptr, OPT_QUERY_RAW},
      {"version", no_argument, nullptr, OPT_VERSION},
      {"save-for-bugreport", no_argument, nullptr, OPT_BUGREPORT},
      {nullptr, 0, nullptr, 0}};

  std::string config_file_name;
  std::string trace_config_raw;
  bool parse_as_pbtxt = false;
  TraceConfig::StatsdMetadata statsd_metadata;
  limiter_.reset(new RateLimiter());

  ConfigOptions config_options;
  bool has_config_options = false;

  if (argc <= 1) {
    PrintUsage(argv[0]);
    return 1;
  }

  for (;;) {
    int option =
        getopt_long(argc, argv, "hc:o:dDt:b:s:a:", long_options, nullptr);

    if (option == -1)
      break;  // EOF.

    if (option == 'c') {
      config_file_name = std::string(optarg);
      if (strcmp(optarg, "-") == 0) {
        std::istreambuf_iterator<char> begin(std::cin), end;
        trace_config_raw.assign(begin, end);
      } else if (strcmp(optarg, ":test") == 0) {
        TraceConfig test_config;
        ConfigOptions opts;
        opts.time = "2s";
        opts.categories.emplace_back("sched/sched_switch");
        opts.categories.emplace_back("power/cpu_idle");
        opts.categories.emplace_back("power/cpu_frequency");
        opts.categories.emplace_back("power/gpu_frequency");
        PERFETTO_CHECK(CreateConfigFromOptions(opts, &test_config));
        trace_config_raw = test_config.SerializeAsString();
      } else {
        if (!base::ReadFile(optarg, &trace_config_raw)) {
          PERFETTO_PLOG("Could not open %s", optarg);
          return 1;
        }
      }
      continue;
    }

    if (option == 'o') {
      trace_out_path_ = optarg;
      continue;
    }

    if (option == 'd') {
      background_ = true;
      continue;
    }

    if (option == 'D') {
      background_ = true;
      background_wait_ = true;
      continue;
    }

    if (option == 't') {
      has_config_options = true;
      config_options.time = std::string(optarg);
      continue;
    }

    if (option == 'b') {
      has_config_options = true;
      config_options.buffer_size = std::string(optarg);
      continue;
    }

    if (option == 's') {
      has_config_options = true;
      config_options.max_file_size = std::string(optarg);
      continue;
    }

    if (option == 'a') {
      config_options.atrace_apps.push_back(std::string(optarg));
      has_config_options = true;
      continue;
    }

    if (option == OPT_UPLOAD) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
      upload_flag_ = true;
      continue;
#else
      PERFETTO_ELOG("--upload is only supported on Android");
      return 1;
#endif
    }

    if (option == OPT_DROPBOX) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
      PERFETTO_CHECK(optarg);
      upload_flag_ = true;
      continue;
#else
      PERFETTO_ELOG("--dropbox is only supported on Android");
      return 1;
#endif
    }

    if (option == OPT_PBTXT_CONFIG) {
      parse_as_pbtxt = true;
      continue;
    }

    if (option == OPT_IGNORE_GUARDRAILS) {
      ignore_guardrails_ = true;
      continue;
    }

    if (option == OPT_RESET_GUARDRAILS) {
      PERFETTO_CHECK(limiter_->ClearState());
      PERFETTO_ILOG("Guardrail state cleared");
      return 0;
    }

    if (option == OPT_ALERT_ID) {
      statsd_metadata.set_triggering_alert_id(atoll(optarg));
      continue;
    }

    if (option == OPT_CONFIG_ID) {
      statsd_metadata.set_triggering_config_id(atoll(optarg));
      continue;
    }

    if (option == OPT_CONFIG_UID) {
      statsd_metadata.set_triggering_config_uid(atoi(optarg));
      continue;
    }

    if (option == OPT_SUBSCRIPTION_ID) {
      statsd_metadata.set_triggering_subscription_id(atoll(optarg));
      continue;
    }

    if (option == OPT_DETACH) {
      detach_key_ = std::string(optarg);
      PERFETTO_CHECK(!detach_key_.empty());
      continue;
    }

    if (option == OPT_ATTACH) {
      attach_key_ = std::string(optarg);
      PERFETTO_CHECK(!attach_key_.empty());
      continue;
    }

    if (option == OPT_IS_DETACHED) {
      attach_key_ = std::string(optarg);
      redetach_once_attached_ = true;
      PERFETTO_CHECK(!attach_key_.empty());
      continue;
    }

    if (option == OPT_STOP) {
      stop_trace_once_attached_ = true;
      continue;
    }

    if (option == OPT_QUERY) {
      query_service_ = true;
      continue;
    }

    if (option == OPT_QUERY_RAW) {
      query_service_ = true;
      query_service_output_raw_ = true;
      continue;
    }

    if (option == OPT_VERSION) {
      printf("%s\n", base::GetVersionString());
      return 0;
    }

    if (option == OPT_BUGREPORT) {
      bugreport_ = true;
      continue;
    }

    PrintUsage(argv[0]);
    return 1;
  }

  for (ssize_t i = optind; i < argc; i++) {
    has_config_options = true;
    config_options.categories.push_back(argv[i]);
  }

  if (query_service_ && (is_detach() || is_attach() || background_)) {
    PERFETTO_ELOG("--query cannot be combined with any other argument");
    return 1;
  }

  if (is_detach() && is_attach()) {
    PERFETTO_ELOG("--attach and --detach are mutually exclusive");
    return 1;
  }

  if (is_detach() && background_) {
    PERFETTO_ELOG("--detach and --background are mutually exclusive");
    return 1;
  }

  if (stop_trace_once_attached_ && !is_attach()) {
    PERFETTO_ELOG("--stop is supported only in combination with --attach");
    return 1;
  }

  if (bugreport_ && (is_attach() || is_detach() || query_service_ ||
                     has_config_options || background_wait_)) {
    PERFETTO_ELOG("--save-for-bugreport cannot take any other argument");
    return 1;
  }

  // Parse the trace config. It can be either:
  // 1) A proto-encoded file/stdin (-c ...).
  // 2) A proto-text file/stdin (-c ... --txt).
  // 3) A set of option arguments (-t 10s -s 10m).
  // The only cases in which a trace config is not expected is --attach.
  // For this we are just acting on already existing sessions.
  trace_config_.reset(new TraceConfig());

  bool parsed = false;
  const bool will_trace = !is_attach() && !query_service_ && !bugreport_;
  if (!will_trace) {
    if ((!trace_config_raw.empty() || has_config_options)) {
      PERFETTO_ELOG("Cannot specify a trace config with this option");
      return 1;
    }
  } else if (has_config_options) {
    if (!trace_config_raw.empty()) {
      PERFETTO_ELOG(
          "Cannot specify both -c/--config and any of --time, --size, "
          "--buffer, --app, ATRACE_CAT, FTRACE_EVENT");
      return 1;
    }
    parsed = CreateConfigFromOptions(config_options, trace_config_.get());
  } else {
    if (trace_config_raw.empty()) {
      PERFETTO_ELOG("The TraceConfig is empty");
      return 1;
    }
    PERFETTO_DLOG("Parsing TraceConfig, %zu bytes", trace_config_raw.size());
    if (parse_as_pbtxt) {
      parsed = ParseTraceConfigPbtxt(config_file_name, trace_config_raw,
                                     trace_config_.get());
    } else {
      parsed = trace_config_->ParseFromString(trace_config_raw);
    }
  }

  if (parsed) {
    *trace_config_->mutable_statsd_metadata() = std::move(statsd_metadata);
    trace_config_raw.clear();
  } else if (will_trace) {
    PERFETTO_ELOG("The trace config is invalid, bailing out.");
    return 1;
  }

  if (trace_config_->trace_uuid_lsb() == 0 &&
      trace_config_->trace_uuid_msb() == 0) {
    base::Uuid uuid = base::Uuidv4();
    if (trace_config_->statsd_metadata().triggering_subscription_id()) {
      uuid.set_lsb(
          trace_config_->statsd_metadata().triggering_subscription_id());
    }
    uuid_ = uuid.ToString();
    trace_config_->set_trace_uuid_msb(uuid.msb());
    trace_config_->set_trace_uuid_lsb(uuid.lsb());
  } else {
    base::Uuid uuid(trace_config_->trace_uuid_lsb(),
                    trace_config_->trace_uuid_msb());
    uuid_ = uuid.ToString();
  }

  if (!trace_config_->incident_report_config().destination_package().empty() &&
      !upload_flag_) {
    PERFETTO_ELOG(
        "Unexpected IncidentReportConfig without --dropbox / --upload.");
    return 1;
  }

  if (trace_config_->activate_triggers().empty() &&
      trace_config_->incident_report_config().destination_package().empty() &&
      !trace_config_->incident_report_config().skip_incidentd() &&
      upload_flag_) {
    PERFETTO_ELOG(
        "Missing IncidentReportConfig.destination_package with --dropbox / "
        "--upload.");
    return 1;
  }

  // Only save to incidentd if both --upload is set and |skip_incidentd| is
  // absent or false.
  save_to_incidentd_ =
      upload_flag_ && !trace_config_->incident_report_config().skip_incidentd();

  // Respect the wishes of the config with respect to statsd logging or fall
  // back on the presence of the --upload flag if not set.
  switch (trace_config_->statsd_logging()) {
    case TraceConfig::STATSD_LOGGING_ENABLED:
      statsd_logging_ = true;
      break;
    case TraceConfig::STATSD_LOGGING_DISABLED:
      statsd_logging_ = false;
      break;
    case TraceConfig::STATSD_LOGGING_UNSPECIFIED:
      statsd_logging_ = upload_flag_;
      break;
  }
  trace_config_->set_statsd_logging(statsd_logging_
                                        ? TraceConfig::STATSD_LOGGING_ENABLED
                                        : TraceConfig::STATSD_LOGGING_DISABLED);

  // Set up the output file. Either --out or --upload are expected, with the
  // only exception of --attach. In this case the output file is passed when
  // detaching.
  if (!trace_out_path_.empty() && upload_flag_) {
    PERFETTO_ELOG(
        "Can't log to a file (--out) and incidentd (--upload) at the same "
        "time");
    return 1;
  }

  if (!trace_config_->output_path().empty()) {
    if (!trace_out_path_.empty() || upload_flag_) {
      PERFETTO_ELOG(
          "Can't pass --out or --upload if output_path is set in the "
          "trace config");
      return 1;
    }
    if (base::FileExists(trace_config_->output_path())) {
      PERFETTO_ELOG(
          "The output_path must not exist, the service cannot overwrite "
          "existing files for security reasons. Remove %s or use a different "
          "path.",
          trace_config_->output_path().c_str());
      return 1;
    }
  }

  // |activate_triggers| in the trace config is shorthand for trigger_perfetto.
  // In this case we don't intend to send any trace config to the service,
  // rather use that as a signal to the cmdline client to connect as a producer
  // and activate triggers.
  if (!trace_config_->activate_triggers().empty()) {
    for (const auto& trigger : trace_config_->activate_triggers()) {
      triggers_to_activate_.push_back(trigger);
    }
    trace_config_.reset(new TraceConfig());
  }

  bool open_out_file = true;
  if (!will_trace) {
    open_out_file = false;
    if (!trace_out_path_.empty() || upload_flag_) {
      PERFETTO_ELOG("Can't pass an --out file (or --upload) with this option");
      return 1;
    }
  } else if (!triggers_to_activate_.empty() ||
             (trace_config_->write_into_file() &&
              !trace_config_->output_path().empty())) {
    open_out_file = false;
  } else if (trace_out_path_.empty() && !upload_flag_) {
    PERFETTO_ELOG("Either --out or --upload is required");
    return 1;
  } else if (is_detach() && !trace_config_->write_into_file()) {
    // In detached mode we must pass the file descriptor to the service and
    // let that one write the trace. We cannot use the IPC readback code path
    // because the client process is about to exit soon after detaching.
    // We could support detach && !write_into_file, but that would make the
    // cmdline logic more complex. The feasible configurations are:
    // 1. Using write_into_file and passing the file path on the --detach call.
    // 2. Using pure ring-buffer mode, setting write_into_file = false and
    //    passing the output file path to the --attach call.
    // This is too complicated and harder to reason about, so we support only 1.
    // Traceur gets around this by always setting write_into_file and specifying
    // file_write_period_ms = 1week (which effectively means: write into the
    // file only at the end of the trace) to achieve ring buffer traces.
    PERFETTO_ELOG(
        "TraceConfig's write_into_file must be true when using --detach");
    return 1;
  }
  if (open_out_file) {
    if (!OpenOutputFile())
      return 1;
    if (!trace_config_->write_into_file())
      packet_writer_ = CreateFilePacketWriter(trace_out_stream_.get());
  }

  if (trace_config_->compression_type() ==
      TraceConfig::COMPRESSION_TYPE_DEFLATE) {
    if (packet_writer_) {
#if PERFETTO_BUILDFLAG(PERFETTO_ZLIB)
      packet_writer_ = CreateZipPacketWriter(std::move(packet_writer_));
#else
      PERFETTO_ELOG("Cannot compress. Zlib not enabled in the build config");
#endif
    } else {
      PERFETTO_ELOG("Cannot compress when tracing directly to file.");
    }
  }

  if (save_to_incidentd_ && !ignore_guardrails_ &&
      (trace_config_->duration_ms() == 0 &&
       trace_config_->trigger_config().trigger_timeout_ms() == 0)) {
    PERFETTO_ELOG("Can't trace indefinitely when tracing to Incidentd.");
    return 1;
  }

  if (background_) {
    if (background_wait_) {
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
      background_wait_pipe_ = base::Pipe::Create(base::Pipe::kRdNonBlock);
#endif
    }

    base::Daemonize([this]() -> int {
      background_wait_pipe_.wr.reset();

      if (background_wait_) {
        return WaitOnBgProcessPipe();
      }

      return 0;
    });
    background_wait_pipe_.rd.reset();
  }

  return base::nullopt;  // Continues in ConnectToServiceRunAndMaybeNotify()
                         // below.
}

void PerfettoCmd::NotifyBgProcessPipe(BgProcessStatus status) {
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  if (!background_wait_pipe_.wr) {
    return;
  }
  static_assert(sizeof status == 1, "Enum bigger than one byte");
  PERFETTO_EINTR(write(background_wait_pipe_.wr.get(), &status, 1));
  background_wait_pipe_.wr.reset();
#else   // PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  base::ignore_result(status);
#endif  //! PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
}

PerfettoCmd::BgProcessStatus PerfettoCmd::WaitOnBgProcessPipe() {
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  base::ScopedPlatformHandle fd = std::move(background_wait_pipe_.rd);
  PERFETTO_CHECK(fd);

  BgProcessStatus msg;
  static_assert(sizeof msg == 1, "Enum bigger than one byte");
  std::array<pollfd, 1> pollfds = {pollfd{fd.get(), POLLIN, 0}};

  int ret = PERFETTO_EINTR(poll(&pollfds[0], pollfds.size(), 30000 /*ms*/));
  PERFETTO_CHECK(ret >= 0);
  if (ret == 0) {
    fprintf(stderr, "Timeout waiting for all data sources to start\n");
    return kBackgroundTimeout;
  }
  ssize_t read_ret = PERFETTO_EINTR(read(fd.get(), &msg, 1));
  PERFETTO_CHECK(read_ret >= 0);
  if (read_ret == 0) {
    fprintf(stderr, "Background process didn't report anything\n");
    return kBackgroundOtherError;
  }

  if (msg != kBackgroundOk) {
    fprintf(stderr, "Background process failed, BgProcessStatus=%d\n",
            static_cast<int>(msg));
    return msg;
  }
#endif  //! PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)

  return kBackgroundOk;
}

int PerfettoCmd::ConnectToServiceRunAndMaybeNotify() {
  int exit_code = ConnectToServiceAndRun();

  NotifyBgProcessPipe(exit_code == 0 ? kBackgroundOk : kBackgroundOtherError);

  return exit_code;
}

int PerfettoCmd::ConnectToServiceAndRun() {
  // If we are just activating triggers then we don't need to rate limit,
  // connect as a consumer or run the trace. So bail out after processing all
  // the options.
  if (!triggers_to_activate_.empty()) {
    LogUploadEvent(PerfettoStatsdAtom::kTriggerBegin);
    LogTriggerEvents(PerfettoTriggerAtom::kCmdTrigger, triggers_to_activate_);

    bool finished_with_success = false;
    TriggerProducer producer(
        &task_runner_,
        [this, &finished_with_success](bool success) {
          finished_with_success = success;
          task_runner_.Quit();
        },
        &triggers_to_activate_);
    task_runner_.Run();
    if (finished_with_success) {
      LogUploadEvent(PerfettoStatsdAtom::kTriggerSuccess);
    } else {
      LogUploadEvent(PerfettoStatsdAtom::kTriggerFailure);
      LogTriggerEvents(PerfettoTriggerAtom::kCmdTriggerFail,
                       triggers_to_activate_);
    }
    return finished_with_success ? 0 : 1;
  }  // if (triggers_to_activate_)

  if (query_service_ || bugreport_) {
    consumer_endpoint_ =
        ConsumerIPCClient::Connect(GetConsumerSocket(), this, &task_runner_);
    task_runner_.Run();
    return 1;  // We can legitimately get here if the service disconnects.
  }            // if (query_service || bugreport_)

  RateLimiter::Args args{};
  args.is_user_build = IsUserBuild();
  args.is_uploading = save_to_incidentd_;
  args.current_time = base::GetWallTimeS();
  args.ignore_guardrails = ignore_guardrails_;
  args.allow_user_build_tracing = trace_config_->allow_user_build_tracing();
  args.unique_session_name = trace_config_->unique_session_name();
  args.max_upload_bytes_override =
      trace_config_->guardrail_overrides().max_upload_per_day_bytes();

  if (!args.unique_session_name.empty())
    base::MaybeSetThreadName("p-" + args.unique_session_name);

  expected_duration_ms_ = trace_config_->duration_ms();
  if (!expected_duration_ms_) {
    uint32_t timeout_ms = trace_config_->trigger_config().trigger_timeout_ms();
    uint32_t max_stop_delay_ms = 0;
    for (const auto& trigger : trace_config_->trigger_config().triggers()) {
      max_stop_delay_ms = std::max(max_stop_delay_ms, trigger.stop_delay_ms());
    }
    expected_duration_ms_ = timeout_ms + max_stop_delay_ms;
  }

  if (trace_config_->trigger_config().trigger_timeout_ms() == 0) {
    LogUploadEvent(PerfettoStatsdAtom::kTraceBegin);
  } else {
    LogUploadEvent(PerfettoStatsdAtom::kBackgroundTraceBegin);
  }

  auto err_atom = ConvertRateLimiterResponseToAtom(limiter_->ShouldTrace(args));
  if (err_atom) {
    // TODO(lalitm): remove this once we're ready on server side.
    LogUploadEvent(PerfettoStatsdAtom::kHitGuardrails);
    LogUploadEvent(err_atom.value());
    return 1;
  }

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  if (!background_ && !is_detach() && !upload_flag_ &&
      triggers_to_activate_.empty() && !isatty(STDIN_FILENO) &&
      !isatty(STDERR_FILENO)) {
    fprintf(stderr,
            "Warning: No PTY. CTRL+C won't gracefully stop the trace. If you "
            "are running perfetto via adb shell, use the -tt arg (adb shell "
            "-t perfetto ...) or consider using the helper script "
            "tools/record_android_trace from the Perfetto repository.\n\n");
  }
#endif

  consumer_endpoint_ =
      ConsumerIPCClient::Connect(GetConsumerSocket(), this, &task_runner_);
  SetupCtrlCSignalHandler();
  task_runner_.Run();

  return limiter_->OnTraceDone(args, update_guardrail_state_, bytes_written_)
             ? 0
             : 1;
}

void PerfettoCmd::OnConnect() {
  LogUploadEvent(PerfettoStatsdAtom::kOnConnect);

  if (background_wait_) {
    consumer_endpoint_->ObserveEvents(
        perfetto::ObservableEvents::TYPE_ALL_DATA_SOURCES_STARTED);
  }

  if (query_service_) {
    consumer_endpoint_->QueryServiceState(
        [this](bool success, const TracingServiceState& svc_state) {
          PrintServiceState(success, svc_state);
          fflush(stdout);
          exit(success ? 0 : 1);
        });
    return;
  }

  if (bugreport_) {
    consumer_endpoint_->SaveTraceForBugreport(
        [](bool success, const std::string& msg) {
          if (success) {
            PERFETTO_ILOG("Trace saved into %s", msg.c_str());
            exit(0);
          }
          PERFETTO_ELOG("%s", msg.c_str());
          exit(1);
        });
    return;
  }

  if (is_attach()) {
    consumer_endpoint_->Attach(attach_key_);
    return;
  }

  if (expected_duration_ms_) {
    PERFETTO_LOG("Connected to the Perfetto traced service, TTL: %ds",
                 (expected_duration_ms_ + 999) / 1000);
  } else {
    PERFETTO_LOG("Connected to the Perfetto traced service, starting tracing");
  }

  PERFETTO_DCHECK(trace_config_);
  trace_config_->set_enable_extra_guardrails(save_to_incidentd_);

  // Set the statsd logging flag if we're uploading

  base::ScopedFile optional_fd;
  if (trace_config_->write_into_file() && trace_config_->output_path().empty())
    optional_fd.reset(dup(fileno(*trace_out_stream_)));

  consumer_endpoint_->EnableTracing(*trace_config_, std::move(optional_fd));

  if (is_detach()) {
    consumer_endpoint_->Detach(detach_key_);  // Will invoke OnDetach() soon.
    return;
  }

  // Failsafe mechanism to avoid waiting indefinitely if the service hangs.
  if (expected_duration_ms_) {
    uint32_t trace_timeout = expected_duration_ms_ + 60000 +
                             trace_config_->flush_timeout_ms() +
                             trace_config_->data_source_stop_timeout_ms();
    task_runner_.PostDelayedTask(std::bind(&PerfettoCmd::OnTimeout, this),
                                 trace_timeout);
  }
}

void PerfettoCmd::OnDisconnect() {
  PERFETTO_LOG("Disconnected from the Perfetto traced service");
  task_runner_.Quit();
}

void PerfettoCmd::OnTimeout() {
  PERFETTO_ELOG("Timed out while waiting for trace from the service, aborting");
  LogUploadEvent(PerfettoStatsdAtom::kOnTimeout);
  task_runner_.Quit();
}

void PerfettoCmd::CheckTraceDataTimeout() {
  if (trace_data_timeout_armed_) {
    PERFETTO_ELOG("Timed out while waiting for OnTraceData, aborting");
    FinalizeTraceAndExit();
  }
  trace_data_timeout_armed_ = true;
  task_runner_.PostDelayedTask(
      std::bind(&PerfettoCmd::CheckTraceDataTimeout, this),
      kOnTraceDataTimeoutMs);
}

void PerfettoCmd::OnTraceData(std::vector<TracePacket> packets, bool has_more) {
  trace_data_timeout_armed_ = false;

  if (!packet_writer_->WritePackets(packets)) {
    PERFETTO_ELOG("Failed to write packets");
    FinalizeTraceAndExit();
  }

  if (!has_more)
    FinalizeTraceAndExit();  // Reached end of trace.
}

void PerfettoCmd::OnTracingDisabled(const std::string& error) {
  if (!error.empty()) {
    // Some of these errors (e.g. unique session name already exists) are soft
    // errors and likely to happen in nominal condition. As such they shouldn't
    // be marked as "E" in the event log. Hence why LOG and not ELOG here.
    PERFETTO_LOG("Service error: %s", error.c_str());

    // Update guardrail state even if we failed. This is for two
    // reasons:
    // 1. Keeps compatibility with pre-stats code which used to
    // ignore errors from the service and always update state.
    // 2. We want to prevent failure storms and the guardrails help
    // by preventing tracing too frequently with the same session.
    update_guardrail_state_ = true;
    task_runner_.Quit();
    return;
  }

  // Make sure to only log this atom if |error| is empty; traced
  // would have logged a terminal error atom corresponding to |error|
  // and we don't want to log anything after that.
  LogUploadEvent(PerfettoStatsdAtom::kOnTracingDisabled);

  if (trace_config_->write_into_file()) {
    // If write_into_file == true, at this point the passed file contains
    // already all the packets.
    return FinalizeTraceAndExit();
  }

  trace_data_timeout_armed_ = false;
  CheckTraceDataTimeout();

  // This will cause a bunch of OnTraceData callbacks. The last one will
  // save the file and exit.
  consumer_endpoint_->ReadBuffers();
}

void PerfettoCmd::FinalizeTraceAndExit() {
  LogUploadEvent(PerfettoStatsdAtom::kFinalizeTraceAndExit);
  packet_writer_.reset();

  if (trace_out_stream_) {
    fseek(*trace_out_stream_, 0, SEEK_END);
    off_t sz = ftell(*trace_out_stream_);
    if (sz > 0)
      bytes_written_ = static_cast<size_t>(sz);
  }

  if (save_to_incidentd_) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
    SaveTraceIntoDropboxAndIncidentOrCrash();
#endif
  } else {
    trace_out_stream_.reset();
    if (trace_config_->write_into_file()) {
      // trace_out_path_ might be empty in the case of --attach.
      PERFETTO_LOG("Trace written into the output file");
    } else {
      PERFETTO_LOG("Wrote %" PRIu64 " bytes into %s", bytes_written_,
                   trace_out_path_ == "-" ? "stdout" : trace_out_path_.c_str());
    }
  }

  update_guardrail_state_ = true;
  task_runner_.Quit();
}

bool PerfettoCmd::OpenOutputFile() {
  base::ScopedFile fd;
  if (trace_out_path_.empty()) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
    fd = CreateUnlinkedTmpFile();
#endif
  } else if (trace_out_path_ == "-") {
    fd.reset(dup(fileno(stdout)));
  } else {
    fd = base::OpenFile(trace_out_path_, O_RDWR | O_CREAT | O_TRUNC, 0600);
  }
  if (!fd) {
    PERFETTO_PLOG(
        "Failed to open %s. If you get permission denied in "
        "/data/misc/perfetto-traces, the file might have been "
        "created by another user, try deleting it first.",
        trace_out_path_.c_str());
    return false;
  }
  trace_out_stream_.reset(fdopen(fd.release(), "wb"));
  PERFETTO_CHECK(trace_out_stream_);
  return true;
}

void PerfettoCmd::SetupCtrlCSignalHandler() {
  base::InstallCtrCHandler([] { g_perfetto_cmd->SignalCtrlC(); });
  task_runner_.AddFileDescriptorWatch(ctrl_c_evt_.fd(), [this] {
    PERFETTO_LOG("SIGINT/SIGTERM received: disabling tracing.");
    ctrl_c_evt_.Clear();
    consumer_endpoint_->Flush(0, [this](bool flush_success) {
      if (!flush_success)
        PERFETTO_ELOG("Final flush unsuccessful.");
      consumer_endpoint_->DisableTracing();
    });
  });
}

void PerfettoCmd::OnDetach(bool success) {
  if (!success) {
    PERFETTO_ELOG("Session detach failed");
    exit(1);
  }
  exit(0);
}

void PerfettoCmd::OnAttach(bool success, const TraceConfig& trace_config) {
  if (!success) {
    if (!redetach_once_attached_) {
      // Print an error message if attach fails, with the exception of the
      // --is_detached case, where we want to silently return.
      PERFETTO_ELOG("Session re-attach failed. Check service logs for details");
    }
    // Keep this exit code distinguishable from the general error code so
    // --is_detached can tell the difference between a general error and the
    // not-detached case.
    exit(2);
  }

  if (redetach_once_attached_) {
    consumer_endpoint_->Detach(attach_key_);  // Will invoke OnDetach() soon.
    return;
  }

  trace_config_.reset(new TraceConfig(trace_config));
  PERFETTO_DCHECK(trace_config_->write_into_file());

  if (stop_trace_once_attached_) {
    consumer_endpoint_->Flush(0, [this](bool flush_success) {
      if (!flush_success)
        PERFETTO_ELOG("Final flush unsuccessful.");
      consumer_endpoint_->DisableTracing();
    });
  }
}

void PerfettoCmd::OnTraceStats(bool /*success*/,
                               const TraceStats& /*trace_config*/) {
  // TODO(eseckler): Support GetTraceStats().
}

void PerfettoCmd::PrintServiceState(bool success,
                                    const TracingServiceState& svc_state) {
  if (!success) {
    PERFETTO_ELOG("Failed to query the service state");
    return;
  }

  if (query_service_output_raw_) {
    std::string str = svc_state.SerializeAsString();
    fwrite(str.data(), 1, str.size(), stdout);
    return;
  }

  printf("Not meant for machine consumption. Use --query-raw for scripts.\n");

  for (const auto& producer : svc_state.producers()) {
    printf("producers: {\n");
    printf("  id: %d\n", producer.id());
    printf("  name: \"%s\" \n", producer.name().c_str());
    printf("  uid: %d \n", producer.uid());
    printf("  sdk_version: \"%s\" \n", producer.sdk_version().c_str());
    printf("}\n");
  }

  for (const auto& ds : svc_state.data_sources()) {
    printf("data_sources: {\n");
    printf("  producer_id: %d\n", ds.producer_id());
    printf("  descriptor: {\n");
    printf("    name: \"%s\"\n", ds.ds_descriptor().name().c_str());
    printf("  }\n");
    printf("}\n");
  }
  printf("tracing_service_version: \"%s\"\n",
         svc_state.tracing_service_version().c_str());
  printf("num_sessions: %d\n", svc_state.num_sessions());
  printf("num_sessions_started: %d\n", svc_state.num_sessions_started());
}

void PerfettoCmd::OnObservableEvents(
    const ObservableEvents& observable_events) {
  if (observable_events.all_data_sources_started()) {
    NotifyBgProcessPipe(kBackgroundOk);
  }
}

void PerfettoCmd::LogUploadEvent(PerfettoStatsdAtom atom) {
  if (!statsd_logging_)
    return;
  base::Uuid uuid(uuid_);
  android_stats::MaybeLogUploadEvent(atom, uuid.lsb(), uuid.msb());
}

void PerfettoCmd::LogTriggerEvents(
    PerfettoTriggerAtom atom,
    const std::vector<std::string>& trigger_names) {
  if (!statsd_logging_)
    return;
  android_stats::MaybeLogTriggerEvents(atom, trigger_names);
}

int PERFETTO_EXPORT_ENTRYPOINT PerfettoCmdMain(int argc, char** argv) {
  perfetto::PerfettoCmd cmd;
  auto opt_res = cmd.ParseCmdlineAndMaybeDaemonize(argc, argv);
  if (opt_res.has_value())
    return *opt_res;
  return cmd.ConnectToServiceRunAndMaybeNotify();
}

}  // namespace perfetto
