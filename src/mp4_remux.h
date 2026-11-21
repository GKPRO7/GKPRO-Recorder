#pragma once
// Built-in MKV -> MP4 remuxer for GKPRO Recorder files (no re-encoding, so it is fast and lossless).
// Also repairs recordings that were cut off by a crash.

#include <string>

bool RemuxMkvToMp4(const std::wstring& mkvPath, const std::wstring& mp4Path, std::wstring& error);
