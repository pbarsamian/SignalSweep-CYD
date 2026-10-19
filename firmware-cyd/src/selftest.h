// SPDX-License-Identifier: GPL-3.0-or-later
// selftest.h — M1 acceptance checks for the shared contract, run on-device at boot
// (no host compiler is needed). Prints one line per check to the serial monitor.
#pragma once

struct SelfTestResult { int passed; int total; };
SelfTestResult selftest_run();
