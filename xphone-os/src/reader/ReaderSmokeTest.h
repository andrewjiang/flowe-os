#pragma once

// TEMPORARY stage-1 smoke hook. Compiled but not called anywhere yet.
// Opens an epub, builds (or loads) section 0's cache, deserializes page 0 and
// Serial.printf's page/line/word counts. Call from a debug path on device:
//   readerSmokeTest("/books/alice.epub");
bool readerSmokeTest(const char* epubPath);
