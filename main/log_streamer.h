#ifndef _LOG_STREAMER_H
#define _LOG_STREAMER_H


#define UART_LOGE(tag, fmt, ...) log_no_stream("E", tag, fmt, ##__VA_ARGS__)
#define UART_LOGW(tag, fmt, ...) log_no_stream("W", tag, fmt, ##__VA_ARGS__)
#define UART_LOGI(tag, fmt, ...) log_no_stream("I", tag, fmt, ##__VA_ARGS__)
#define UART_LOGD(tag, fmt, ...) log_no_stream("D", tag, fmt, ##__VA_ARGS__)
#define UART_LOGV(tag, fmt, ...) log_no_stream("V", tag, fmt, ##__VA_ARGS__)

int log_no_stream(const char* level, const char* tag, const char* fmt, ...);
void log_streamer_setup(void);

#endif // _LOG_STREAMER_H
