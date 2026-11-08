#include "stdafx.h"
#include "Settings.h"
#include "..\Minecraft.World\StringHelpers.h"

#include <fstream>
#include <string>
#include <sstream>

namespace
{
	std::string TrimAscii(const std::string& value)
	{
		size_t start = value.find_first_not_of(" \t\r\n");
		if (start == std::string::npos) return std::string();
		size_t end = value.find_last_not_of(" \t\r\n");
		return value.substr(start, end - start + 1);
	}
}

// The original console Settings class was a stub (the constructor did
// nothing), so every value in server.properties was silently ignored. Parse
// the standard key=value file here; the dedicated server treats it as
// operator-owned configuration and does not rewrite it at runtime.
Settings::Settings(File *file)
{
	if (file == NULL) return;

	std::ifstream in(wstringtofilename(file->getPath()));
	if (!in.is_open()) return;

	std::string line;
	while (std::getline(in, line))
	{
		size_t start = line.find_first_not_of(" \t\r\n");
		if (start == std::string::npos) continue;
		if (line[start] == '#') continue;

		size_t equals = line.find('=', start);
		if (equals == std::string::npos) continue;

		std::string key = TrimAscii(line.substr(start, equals - start));
		std::string value = TrimAscii(line.substr(equals + 1));
		if (key.empty()) continue;
		properties[convStringToWstring(key)] = convStringToWstring(value);
	}
}

void Settings::generateNewProperties()
{
}

void Settings::saveProperties()
{
	// Read-only: keep comments and operator edits intact.
}

wstring Settings::getString(const wstring& key, const wstring& defaultValue)
{
	auto it = properties.find(key);
	if (it == properties.end()) return defaultValue;
	return it->second;
}

int Settings::getInt(const wstring& key, int defaultValue)
{
	auto it = properties.find(key);
	if (it == properties.end()) return defaultValue;
	std::wistringstream stream(it->second);
	int value = defaultValue;
	if (!(stream >> value)) return defaultValue;
	return value;
}

bool Settings::getBoolean(const wstring& key, bool defaultValue)
{
	auto it = properties.find(key);
	if (it == properties.end()) return defaultValue;
	const wstring& value = it->second;
	if (value == L"true" || value == L"TRUE" || value == L"1") return true;
	if (value == L"false" || value == L"FALSE" || value == L"0") return false;
	return defaultValue;
}

void Settings::setBooleanAndSave(const wstring& key, bool value)
{
	properties[key] = _toString<bool>(value);
}
