#ifndef _OTA_UPDATER_H
#define _OTA_UPDATER_H

#include <stddef.h>
#include <stdint.h>

class JsonParser;

/**
 * @brief Firmware updates over ThingsBoard's MQTT OTA protocol.
 *
 * ThingsBoard publishes the target firmware as shared attributes (fw_title,
 * fw_version, fw_size, fw_checksum, fw_checksum_algorithm). The device then
 * pulls the image a chunk at a time by publishing to
 * `v2/fw/request/<request_id>/chunk/<n>` and reading the reply on
 * `v2/fw/response/<request_id>/chunk/<n>`, reporting progress back as an
 * `fw_state` telemetry value.
 *
 * The image is written straight into the inactive OTA slot as fragments
 * arrive, so nothing buffers a whole chunk and the download is bounded by the
 * partition rather than by RAM.
 */
class OtaUpdater {
public:
	/// Chunk size requested from the broker, in bytes.
	static constexpr size_t CHUNK_SIZE = 4096;

	/// True if `topic` is one this class handles.
	static bool ownsTopic(const char* topic);

	/**
	 * @brief Start the supervisor task.
	 *
	 * It waits for MQTT, confirms a pending image (cancelling the bootloader's
	 * rollback), reports the running version, and afterwards watches for a
	 * download that has stopped making progress.
	 */
	static void begin(void);

	/// Subscribe to the chunk-response topic. Call once MQTT is connected.
	static void subscribe(void);

	/**
	 * @brief Inspect a shared-attributes document for fw_* keys.
	 *
	 * @param jp   Parser holding the document
	 * @param root Node to read the fw_* keys from (the "shared" object when the
	 *             payload is an attributes response)
	 */
	static void onAttributes(JsonParser& jp, int root);

	/**
	 * @brief Feed one MQTT fragment of a chunk response straight to flash.
	 *
	 * @param topic   Full response topic, used for the chunk index
	 * @param data    Fragment bytes
	 * @param len     Fragment length
	 * @param offset  Offset of this fragment within the chunk
	 * @param total   Total chunk length
	 */
	static void onChunkData(const char* topic, const uint8_t* data, size_t len,
							size_t offset, size_t total);
};

#endif // _OTA_UPDATER_H
