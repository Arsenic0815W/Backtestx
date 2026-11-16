#pragma once
// The report's HTML / CSS / JavaScript, embedded at configure time from src/report_assets/ (see CMakeLists.txt; the definitions
// live in the generated build/generated/report_assets.cpp). CRLF is normalised to LF so the bytes do not depend on how a file was saved.
namespace bt {
namespace report_assets {
const char* report_template();
const char* report_css();
const char* report_js();
}  // namespace report_assets
}  // namespace bt
