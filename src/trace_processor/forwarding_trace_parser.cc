/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/forwarding_trace_parser.h"

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/ninja/ninja_log_parser.h"
#include "src/trace_processor/importers/proto/proto_trace_parser.h"
#include "src/trace_processor/importers/proto/proto_trace_reader.h"
#include "src/trace_processor/trace_sorter.h"

namespace perfetto {
namespace trace_processor {
namespace {

const char kNoZlibErr[] =
    "Cannot open compressed trace. zlib not enabled in the build config";

inline bool isspace(unsigned char c) {
  return ::isspace(c);
}

std::string RemoveWhitespace(std::string str) {
  str.erase(std::remove_if(str.begin(), str.end(), isspace), str.end());
  return str;
}

TraceSorter::SortingMode ConvertSortingMode(SortingMode sorting_mode) {
  switch (sorting_mode) {
    case SortingMode::kDefaultHeuristics:
    case SortingMode::kForceFlushPeriodWindowedSort:
      return TraceSorter::SortingMode::kDefault;
    case SortingMode::kForceFullSort:
      return TraceSorter::SortingMode::kFullSort;
  }
  PERFETTO_FATAL("For GCC");
}

// Fuchsia traces have a magic number as documented here:
// https://fuchsia.googlesource.com/fuchsia/+/HEAD/docs/development/tracing/trace-format/README.md#magic-number-record-trace-info-type-0
constexpr uint64_t kFuchsiaMagicNumber = 0x0016547846040010;

}  // namespace

ForwardingTraceParser::ForwardingTraceParser(TraceProcessorContext* context)
    : context_(context) {}

ForwardingTraceParser::~ForwardingTraceParser() {}

util::Status ForwardingTraceParser::Parse(TraceBlobView blob) {
  // If this is the first Parse() call, guess the trace type and create the
  // appropriate parser.
  if (!reader_) {
    TraceType trace_type;
    {
      auto scoped_trace = context_->storage->TraceExecutionTimeIntoStats(
          stats::guess_trace_type_duration_ns);
      trace_type = GuessTraceType(blob.data(), blob.size());
    }
    switch (trace_type) {
      case kJsonTraceType: {
        PERFETTO_DLOG("JSON trace detected");
        if (context_->json_trace_tokenizer && context_->json_trace_parser) {
          reader_ = std::move(context_->json_trace_tokenizer);

          // JSON traces have no guarantees about the order of events in them.
          context_->sorter.reset(
              new TraceSorter(context_, std::move(context_->json_trace_parser),
                              TraceSorter::SortingMode::kFullSort));
        } else {
          return util::ErrStatus("JSON support is disabled");
        }
        break;
      }
      case kProtoTraceType: {
        PERFETTO_DLOG("Proto trace detected");
        auto sorting_mode = ConvertSortingMode(context_->config.sorting_mode);
        reader_.reset(new ProtoTraceReader(context_));
        context_->sorter.reset(new TraceSorter(
            context_,
            std::unique_ptr<TraceParser>(new ProtoTraceParser(context_)),
            sorting_mode));
        context_->process_tracker->SetPidZeroIgnoredForIdleProcess();
        break;
      }
      case kNinjaLogTraceType: {
        PERFETTO_DLOG("Ninja log detected");
        reader_.reset(new NinjaLogParser(context_));
        break;
      }
      case kFuchsiaTraceType: {
        PERFETTO_DLOG("Fuchsia trace detected");
        if (context_->fuchsia_trace_parser &&
            context_->fuchsia_trace_tokenizer) {
          reader_ = std::move(context_->fuchsia_trace_tokenizer);

          // Fuschia traces can have massively out of order events.
          context_->sorter.reset(new TraceSorter(
              context_, std::move(context_->fuchsia_trace_parser),
              TraceSorter::SortingMode::kFullSort));
        } else {
          return util::ErrStatus("Fuchsia support is disabled");
        }
        break;
      }
      case kSystraceTraceType:
        PERFETTO_DLOG("Systrace trace detected");
        context_->process_tracker->SetPidZeroIgnoredForIdleProcess();
        if (context_->systrace_trace_parser) {
          reader_ = std::move(context_->systrace_trace_parser);
          break;
        } else {
          return util::ErrStatus("Systrace support is disabled");
        }
      case kGzipTraceType:
      case kCtraceTraceType:
        if (trace_type == kGzipTraceType) {
          PERFETTO_DLOG("gzip trace detected");
        } else {
          PERFETTO_DLOG("ctrace trace detected");
        }
        if (context_->gzip_trace_parser) {
          reader_ = std::move(context_->gzip_trace_parser);
          break;
        } else {
          return util::ErrStatus(kNoZlibErr);
        }
      case kUnknownTraceType:
        // If renaming this error message don't remove the "(ERR:fmt)" part.
        // The UI's error_dialog.ts uses it to make the dialog more graceful.
        return util::ErrStatus("Unknown trace type provided (ERR:fmt)");
    }
  }

  return reader_->Parse(std::move(blob));
}

void ForwardingTraceParser::NotifyEndOfFile() {
  reader_->NotifyEndOfFile();
}

TraceType GuessTraceType(const uint8_t* data, size_t size) {
  if (size == 0)
    return kUnknownTraceType;
  std::string start(reinterpret_cast<const char*>(data),
                    std::min<size_t>(size, 32));
  if (size >= 8) {
    uint64_t first_word;
    memcpy(&first_word, data, sizeof(first_word));
    if (first_word == kFuchsiaMagicNumber)
      return kFuchsiaTraceType;
  }
  std::string start_minus_white_space = RemoveWhitespace(start);
  if (base::StartsWith(start_minus_white_space, "{\""))
    return kJsonTraceType;
  if (base::StartsWith(start_minus_white_space, "[{\""))
    return kJsonTraceType;

  // Systrace with header but no leading HTML.
  if (base::Contains(start, "# tracer"))
    return kSystraceTraceType;

  // Systrace with leading HTML.
  if (base::StartsWith(start, "<!DOCTYPE html>") ||
      base::StartsWith(start, "<html>"))
    return kSystraceTraceType;

  // Ctrace is deflate'ed systrace.
  if (base::Contains(start, "TRACE:"))
    return kCtraceTraceType;

  // Ninja's buils log (.ninja_log).
  if (base::StartsWith(start, "# ninja log"))
    return kNinjaLogTraceType;

  // Systrace with no header or leading HTML.
  if (base::StartsWith(start, " "))
    return kSystraceTraceType;

  // gzip'ed trace containing one of the other formats.
  if (base::StartsWith(start, "\x1f\x8b"))
    return kGzipTraceType;

  if (base::StartsWith(start, "\x0a"))
    return kProtoTraceType;

  return kUnknownTraceType;
}

}  // namespace trace_processor
}  // namespace perfetto
