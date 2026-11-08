#pragma once

#include <string>

// Lightweight protocol bots for load testing the dedicated server.
//
// Each bot is a plain TCP client that performs the normal PreLogin/Login
// handshake, answers keep-alives and streams MovePlayer packets. It does not
// create a window, a D3D device or a world, so hundreds of them can run in a
// single process.
void RunProtocolBots(int count, const std::wstring& prefix,
	const std::wstring& host, int port);
