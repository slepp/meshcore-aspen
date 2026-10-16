#pragma once
// The framed transport records its last write time without scheduling work.
inline unsigned long millis() { return 0; }
