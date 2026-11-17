#pragma once
#include <Arduino.h>
#include <vector>

struct Rgb { uint8_t r, g, b; };
struct Roll { String fam, finish, name, color, where; Rgb c1, c2; bool two; int grams, pct, qty; };
struct Slot { String where, type; Rgb c; int pct; };
struct Printer { String name, state, job; bool online; int progress; std::vector<Slot> slots; };
struct Finish { String name; int count; };
// first/count: this material's rolls in Feed::list (the list is sorted by material).
struct Family { String name; int rolls, grams; std::vector<Finish> finishes; std::vector<Rgb> dots; int first = 0, count = 0; };
// Navigation grid: column 0 = home, column N = material N-1.
// Row 0 of a material column is its title card, rows 1.. are its rolls.
struct Feed {
  int rolls = 0, grams = 0, materials = 0;
  std::vector<Printer> printers;
  std::vector<Family> families;
  std::vector<Roll> list;
};
