# Host tests for the target-independent parsing helpers.
#
#   make -C test/host
#
# These build the real main/ sources against a host compiler with ASan and
# UBSan. They cover the code that parses untrusted input -- MQTT payloads and
# log text -- where a bounds bug is a memory-safety bug, not a formatting one.

MAIN := ../../main
CXXFLAGS := -std=c++17 -g -O1 -Wall -Wextra -Wno-unused-parameter \
            -fsanitize=address,undefined -fno-omit-frame-pointer

TESTS := test_json_parser test_util test_json_builder test_ventilation_policy

.PHONY: all clean
all: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

test_json_parser: test_json_parser.cpp $(MAIN)/JsonParser/JsonParser.cpp
	$(CXX) $(CXXFLAGS) -I$(MAIN)/JsonParser $^ -o $@

test_util: test_util.cpp $(MAIN)/util.cpp
	$(CXX) $(CXXFLAGS) -I$(MAIN) $^ -o $@

# stubs/ supplies a minimal esp_log.h; JsonBuilder.h needs only esp_log_level_t.
test_json_builder: test_json_builder.cpp $(MAIN)/JsonBuilder/JsonBuilder.cpp
	$(CXX) $(CXXFLAGS) -I$(MAIN)/JsonBuilder -Istubs $^ -o $@

# The ventilation policy is deliberately free of ESP-IDF and FreeRTOS, so it
# compiles here as-is -- no stubs needed.
test_ventilation_policy: test_ventilation_policy.cpp $(MAIN)/control/VentilationPolicy.cpp
	$(CXX) $(CXXFLAGS) -I$(MAIN) $^ -o $@

clean:
	rm -rf $(TESTS) *.dSYM
