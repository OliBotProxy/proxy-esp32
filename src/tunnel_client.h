#pragma once

// Run one tunnel session: fetch proxy address, connect, serve requests.
// Returns when the session ends (disconnect, GOAWAY, or PONG timeout).
// The caller is responsible for reconnect delay and retry.
void runTunnelSession();
