// Copyright (c) 2013, Cloudera, inc.
// Confidential Cloudera Information: Covered by NDA.

#include <boost/foreach.hpp>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <iostream>
#include <vector>

#include "kudu/common/row_operations.h"
#include "kudu/common/wire_protocol.h"
#include "kudu/consensus/consensus.pb.h"
#include "kudu/consensus/log_index.h"
#include "kudu/consensus/log_reader.h"
#include "kudu/gutil/stl_util.h"
#include "kudu/gutil/strings/numbers.h"
#include "kudu/util/env.h"
#include "kudu/util/flags.h"
#include "kudu/util/logging.h"
#include "kudu/util/metrics.h"
#include "kudu/util/pb_util.h"

DEFINE_bool(print_headers, true, "print the log segment headers/footers");
DEFINE_string(print_entries, "decoded",
              "How to print entries:\n"
              "  false|0|no = don't print\n"
              "  true|1|yes|decoded = print them decoded\n"
              "  pb = print the raw protobuf\n"
              "  id = print only their ids");
DEFINE_int32(truncate_data, 100,
             "Truncate the data fields to the given number of bytes "
             "before printing. Set to 0 to disable");
namespace kudu {
namespace log {

using consensus::CommitMsg;
using consensus::OperationType;
using consensus::ReplicateMsg;
using tserver::WriteRequestPB;
using std::string;
using std::vector;
using std::cout;
using std::endl;

enum PrintEntryType {
  DONT_PRINT,
  PRINT_PB,
  PRINT_DECODED,
  PRINT_ID
};

static PrintEntryType ParsePrintType() {
  if (ParseLeadingBoolValue(FLAGS_print_entries.c_str(), true) == false) {
    return DONT_PRINT;
  } else if (ParseLeadingBoolValue(FLAGS_print_entries.c_str(), false) == true ||
             FLAGS_print_entries == "decoded") {
    return PRINT_DECODED;
  } else if (FLAGS_print_entries == "pb") {
    return PRINT_PB;
  } else if (FLAGS_print_entries == "id") {
    return PRINT_ID;
  } else {
    LOG(FATAL) << "Unknown value for --print_entries: " << FLAGS_print_entries;
  }
}

void PrintIdOnly(const LogEntryPB& entry) {
  switch (entry.type()) {
    case log::REPLICATE:
    {
      cout << entry.replicate().id().term() << "." << entry.replicate().id().index()
           << "@" << entry.replicate().timestamp() << "\t";
      cout << "REPLICATE "
           << OperationType_Name(entry.replicate().op_type());
      break;
    }
    case log::COMMIT:
    {
      cout << "COMMIT " << entry.commit().commited_op_id().term()
           << "." << entry.commit().commited_op_id().index();
      break;
    }
    default:
      cout << "UNKNOWN: " << entry.ShortDebugString();
  }

  cout << endl;
}

void PrintDecodedWriteRequestPB(const string& indent,
                                const Schema& tablet_schema,
                                const WriteRequestPB& write) {
  Schema request_schema;
  CHECK_OK(SchemaFromPB(write.schema(), &request_schema));

  Arena arena(32 * 1024, 1024 * 1024);
  RowOperationsPBDecoder dec(&write.row_operations(), &request_schema, &tablet_schema, &arena);
  vector<DecodedRowOperation> ops;
  CHECK_OK(dec.DecodeOperations(&ops));

  cout << indent << "Tablet: " << write.tablet_id() << endl;
  cout << indent << "Consistency: "
       << ExternalConsistencyMode_Name(write.external_consistency_mode()) << endl;
  if (write.has_propagated_timestamp()) {
    cout << indent << "Propagated TS: " << write.propagated_timestamp() << endl;
  }

  int i = 0;
  BOOST_FOREACH(const DecodedRowOperation& op, ops) {
    // TODO (KUDU-515): Handle the case when a tablet's schema changes
    // mid-segment.
    cout << indent << "op " << (i++) << ": " << op.ToString(tablet_schema) << endl;
  }
}

void PrintDecoded(const LogEntryPB& entry, const Schema& tablet_schema) {
  PrintIdOnly(entry);

  const string indent = "\t";
  if (entry.has_replicate()) {
    // We can actually decode REPLICATE messages.

    const ReplicateMsg& replicate = entry.replicate();
    if (replicate.op_type() == consensus::WRITE_OP) {
      PrintDecodedWriteRequestPB(indent, tablet_schema, replicate.write_request());
    } else {
      cout << indent << replicate.ShortDebugString() << endl;
    }
  } else if (entry.has_commit()) {
    // For COMMIT we'll just dump the PB
    cout << indent << entry.commit().ShortDebugString() << endl;
  }
}

void PrintSegment(const scoped_refptr<ReadableLogSegment>& segment) {
  PrintEntryType print_type = ParsePrintType();
  if (FLAGS_print_headers) {
    cout << "Header:\n" << segment->header().DebugString();
  }
  vector<LogEntryPB*> entries;
  CHECK_OK(segment->ReadEntries(&entries));

  if (print_type == DONT_PRINT) return;

  Schema tablet_schema;
  CHECK_OK(SchemaFromPB(segment->header().schema(), &tablet_schema));

  BOOST_FOREACH(LogEntryPB* entry, entries) {

    if (print_type == PRINT_PB) {
      if (FLAGS_truncate_data > 0) {
        pb_util::TruncateFields(entry, FLAGS_truncate_data);
      }

      cout << "Entry:\n" << entry->DebugString();
    } else if (print_type == PRINT_DECODED) {
      PrintDecoded(*entry, tablet_schema);
    } else if (print_type == PRINT_ID) {
      PrintIdOnly(*entry);
    }
  }
  if (FLAGS_print_headers && segment->HasFooter()) {
    cout << "Footer:\n" << segment->footer().DebugString();
  }
}

void DumpLog(const string &tserver_root_path, const string& tablet_oid) {
  Env *env = Env::Default();
  gscoped_ptr<LogReader> reader;
  FsManager fs_manager(env, tserver_root_path);
  CHECK_OK(LogReader::Open(&fs_manager, scoped_refptr<LogIndex>(), tablet_oid,
                           scoped_refptr<MetricEntity>(), &reader));

  SegmentSequence segments;
  CHECK_OK(reader->GetSegmentsSnapshot(&segments));

  BOOST_FOREACH(const scoped_refptr<ReadableLogSegment>& segment, segments) {
    PrintSegment(segment);
  }
}

void DumpSegment(const string &segment_path) {
  Env *env = Env::Default();
  gscoped_ptr<LogReader> reader;
  scoped_refptr<ReadableLogSegment> segment;
  CHECK_OK(ReadableLogSegment::Open(env, segment_path, &segment));
  CHECK(segment);
  PrintSegment(segment);
}

} // namespace log
} // namespace kudu

int main(int argc, char **argv) {
  kudu::ParseCommandLineFlags(&argc, &argv, true);
  if (argc < 2 || argc > 3) {
    std::cerr << "usage: " << argv[0] << " <tserver root path> <tablet_name>"
        " | <log segment path> " << std::endl;
    return 1;
  }
  kudu::InitGoogleLoggingSafe(argv[0]);
  if (argc == 2) {
    if (!kudu::Env::Default()->FileExists(argv[1])) {
      std::cerr << "Specified file \"" << argv[1] << "\" does not exist"
          << std::endl;
      return 1;
    }
    kudu::log::DumpSegment(argv[1]);
  } else {
    kudu::log::DumpLog(argv[1], argv[2]);
  }

  return 0;
}