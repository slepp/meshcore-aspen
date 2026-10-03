#pragma once
void vTaskDelay(unsigned);
int xTaskCreatePinnedToCore(void (*)(void *), const char *, unsigned, void *,
                            unsigned, void *, unsigned);
