#pragma once
#include <Stream.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cctype>
#define HIGH 1
#define LOW 0
#define OUTPUT 1
unsigned long millis();
void delay(unsigned long);
void pinMode(int, int);
void digitalWrite(int, int);
int digitalRead(int);
