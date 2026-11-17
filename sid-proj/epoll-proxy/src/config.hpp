// ===========================================================================
// config.hpp  --  every tunable setting in one struct (filled from the command line)
// ===========================================================================
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Config {
  // ---- listening ----
  uint16_t port = 8080;    // port clients connect to
  int threads = 0;         // worker threads; 0 means "one per CPU core"
  int backlog = 4096;      // how many not-yet-accepted connections the kernel may queue

  // ---- routing ----
  std::string algo = "lc"; // "rr" = round robin, "lc" = least connections, "ketama"
  std::vector<std::string> backends;  // each one looks like "127.0.0.1:9001"

  // ---- timeouts / retries ----
  int connect_timeout_ms = 1000;  // give up connecting to a backend after this long
  int idle_timeout_ms = 60000;    // close a connection with no traffic for this long
  int max_retries = 3;            // how many different backends to try per client

  // ---- active health checks ----
  bool health_enabled = true;
  std::string health_path = "/health";  // we send "GET /health"
  int health_interval_ms = 500;         // pause between probes of one backend
  int health_timeout_ms = 300;          // a probe slower than this counts as failed
  int health_fall = 2;                  // this many failed probes in a row => mark DOWN
  int health_rise = 2;                  // this many good probes in a row  => mark UP

  // ---- circuit breaker (passive: learns from real connection failures) ----
  int breaker_threshold = 3;      // this many connect failures in a row => open the breaker
  int breaker_cooldown_ms = 2000; // stay open this long, then allow ONE trial connection

  // ---- misc ----
  bool stats = false;  // print a per-backend table every 5 seconds
  bool debug = false;  // verbose logging
};
