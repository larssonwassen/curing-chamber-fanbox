#pragma once
#ifdef __cplusplus
extern "C" {
#endif

void ram_log_snapshot(const char* tag);   // e.g. ram_log_snapshot("before wifi");
long unsigned int get_ram_free(void);
long unsigned int get_ram_total(void);

#ifdef __cplusplus
}
#endif
