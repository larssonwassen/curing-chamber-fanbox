#ifndef _ATOMIC_VARIABLE_H
#define _ATOMIC_VARIABLE_H
// ESP-IDF compatible atomic variable implementation

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

/**
 * @brief Thread-safe atomic variable wrapper with semaphore protection
 * @tparam T The type of data to store atomically
 */
template<typename T>
class AtomicVariable {
public:
	/**
	 * @brief Constructor - initializes the semaphore and default value
	 * @param initial_value Initial value for the atomic variable
	 * @param flash_key Optional NVS flash key for persistent storage (nullptr to disable)
	 */
	explicit AtomicVariable(const T& initial_value = T{}, const char* flash_key = nullptr, void (*onChange)(void*) = nullptr, void* onChangeData = nullptr):
		value_(initial_value),
		flash_key_(flash_key),
		onChange_(onChange),
		onChangeData_(onChangeData)
	{
		semaphore_ = xSemaphoreCreateMutex();
		configASSERT(semaphore_);
		
		// Load value from NVS if flash_key is provided
		if (flash_key_ != nullptr) {
			loadFromNVS();
		}
	}

	/**
	 * @brief Destructor - cleans up the semaphore
	 */
	~AtomicVariable() {
		vSemaphoreDelete(semaphore_);
	}

	/**
	 * @brief Copy constructor - not allowed to prevent issues with semaphore ownership
	 */
	AtomicVariable(const AtomicVariable&) = delete;

	/**
	 * @brief Assignment operator - not allowed to prevent issues with semaphore ownership
	 */
	AtomicVariable& operator=(const AtomicVariable&) = delete;

	/**
	 * @brief Thread-safe get operation
	 * @return Copy of the current value
	 */
	T get() const {
		if (xSemaphoreTake(semaphore_, portMAX_DELAY) == pdTRUE) {
			T result = value_;
			xSemaphoreGive(semaphore_);
			return result;
		}
		return T{}; // Return default value if semaphore take fails
	}

	/**
	 * @brief Thread-safe set operation
	 * @param new_value The new value to set
	 */
	void set(const T& new_value) {
		bool changed = false;
		if (xSemaphoreTake(semaphore_, portMAX_DELAY) == pdTRUE) {
			changed = new_value != value_;
			value_ = new_value;
			if (changed && flash_key_ != nullptr) {
				saveToNVS();
			}
			xSemaphoreGive(semaphore_);
		}
		if (changed && onChange_ != nullptr) {
			onChange_(onChangeData_);
		}
	}

	/**
	 * @brief Assignment operator for direct value assignment
	 * @param new_value The new value to assign
	 * @return Reference to this object
	 */
	AtomicVariable& operator=(const T& new_value) {
		set(new_value);
		return *this;
	}

	/**
	 * @brief Implicit conversion operator for getting the value
	 * @return Copy of the current value
	 */
	operator T() const {
		return get();
	}

private:
	mutable T value_;                                           ///< The actual data being protected
	mutable SemaphoreHandle_t semaphore_;                       ///< Mutex semaphore for thread safety
	const char* flash_key_;                                     ///< NVS flash key for persistent storage (nullptr if disabled)
	const char* TAG_ = "ATOMIC_VARIABLES";                      ///< Tag for logging
	void (*onChange_)(void*);                                   ///< Callback function to call when the value changes
	void* onChangeData_;                                         ///< Data to pass to the callback function
	/**
	 * @brief Load value from NVS flash storage
	 */
	void loadFromNVS(void) {
		if (flash_key_ == nullptr) return;

		nvs_handle_t nvs_handle;
		esp_err_t err = nvs_open("atomic_vars", NVS_READONLY, &nvs_handle);
		if (err == ESP_ERR_NVS_NOT_FOUND) {
			// Namespace does not exist yet: first boot, or after an NVS erase.
			// Seed it with the compiled-in default.
			saveToNVS();
			return;
		}
		if (err != ESP_OK) {
			ESP_LOGE(TAG_, "nvs_open(atomic_vars, RO) failed for '%s': %s",
					 flash_key_, esp_err_to_name(err));
			return;
		}

		size_t required_size = sizeof(T);
		T v;
		err = nvs_get_blob(nvs_handle, flash_key_, &v, &required_size);
		nvs_close(nvs_handle);

		if (err == ESP_OK) {
			if (required_size != sizeof(T)) {
				// Stored blob was written by a build with a different type width.
				// Keep the default rather than reinterpreting the bytes.
				ESP_LOGW(TAG_, "Stored size mismatch for '%s' (%u stored, %u expected); keeping default",
						 flash_key_, (unsigned)required_size, (unsigned)sizeof(T));
				saveToNVS();
			} else {
				value_ = v;
			}
		} else if (err == ESP_ERR_NVS_NOT_FOUND) {
			saveToNVS();
		} else {
			ESP_LOGE(TAG_, "nvs_get_blob('%s') failed: %s", flash_key_, esp_err_to_name(err));
		}
	}
	
	/**
	 * @brief Save value to NVS flash storage
	 */
	void saveToNVS(void) {
		if (flash_key_ == nullptr) return;

		nvs_handle_t nvs_handle;
		esp_err_t err = nvs_open("atomic_vars", NVS_READWRITE, &nvs_handle);
		if (err != ESP_OK) {
			ESP_LOGE(TAG_, "nvs_open(atomic_vars, RW) failed for '%s': %s",
					 flash_key_, esp_err_to_name(err));
			return;
		}

		err = nvs_set_blob(nvs_handle, flash_key_, &value_, sizeof(T));
		if (err != ESP_OK) {
			ESP_LOGE(TAG_, "nvs_set_blob('%s') failed: %s", flash_key_, esp_err_to_name(err));
			nvs_close(nvs_handle);
			return;
		}

		// The commit is what actually costs a flash write. Its return value was
		// discarded before, which is why a million of them went unnoticed.
		err = nvs_commit(nvs_handle);
		if (err != ESP_OK) {
			ESP_LOGE(TAG_, "nvs_commit() failed after writing '%s': %s",
					 flash_key_, esp_err_to_name(err));
		}
		nvs_close(nvs_handle);
	}
};

#endif // _ATOMIC_VARIABLE_H
