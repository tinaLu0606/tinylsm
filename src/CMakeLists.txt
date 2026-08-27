add_library(tinylsm STATIC)
add_library(tinylsm::tinylsm ALIAS tinylsm)

target_sources(tinylsm PRIVATE
  db/db.cpp db/db_impl.cpp
  io/file.cpp
  manifest/manifest_codec.cpp manifest/manifest_state.cpp
  memtable/memtable.cpp
  sstable/sstable_builder.cpp sstable/sstable_reader.cpp
  util/coding.cpp util/crc32c.cpp
  wal/wal_reader.cpp wal/wal_record_codec.cpp wal/wal_writer.cpp
)

protobuf_generate(
  TARGET tinylsm
  LANGUAGE cpp
  APPEND_PATH
  PROTOS ${PROJECT_SOURCE_DIR}/proto/manifest.proto
  PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
)

target_include_directories(tinylsm
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  PRIVATE ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(tinylsm PRIVATE protobuf::libprotobuf)
target_compile_features(tinylsm PUBLIC cxx_std_20)
tinylsm_set_warnings(tinylsm)
tinylsm_enable_sanitizers(tinylsm)
