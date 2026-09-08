idf_component_register(
    SRCS 
        "consts.cpp"
        "config/DeviceConfig.cpp"
        "main.cpp"
        "I2C/I2CDevice.cpp"
        "I2C/I2CBus.cpp"
        "I2C/BusIO_Register.cpp"
        "EMC2101/EMC2101.cpp"
        "SHT31/SHT31.cpp"
        "JsonBuilder/JsonBuilder.cpp"
        "JsonParser/JsonParser.cpp"
        "ram_info.cpp"
        "mqtt.cpp"
        "ota/OtaUpdater.cpp"
        "log_streamer.cpp"
        "util.cpp"
    INCLUDE_DIRS
        "."
)

# Disable missing field initializers warning
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
