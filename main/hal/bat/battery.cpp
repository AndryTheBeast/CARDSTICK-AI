/**
 * @file Battery.cpp
 * @brief Battery management class for ADC-based voltage and level reading
 * @author d4rkmen
 * @license Apache License 2.0
 */

#include "battery.h"
#include "esp_log.h"

using namespace HAL;

static const char* TAG = "Battery";

Battery::Battery()
    : _adc1_handle(nullptr), _adc_cali_handle(nullptr), _do_calibration(false), _adc_raw(0), _voltage_mv(0)
{
    init();
}

Battery::~Battery() { deinit(); }

bool Battery::_adc_calibration_init(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t* out_handle)
{
    adc_cali_handle_t handle = NULL;
    esp_err_t ret = ESP_FAIL;
    bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (!calibrated)
    {
        ESP_LOGD(TAG, "calibration scheme version is %s", "Curve Fitting");
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = unit,
            .chan = channel,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_curve_fitting(&cali_config, &handle);
        if (ret == ESP_OK)
        {
            calibrated = true;
        }
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!calibrated)
    {
        ESP_LOGD(TAG, "calibration scheme version is %s", "Line Fitting");
        adc_cali_line_fitting_config_t cali_config = {
            .unit_id = unit,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_line_fitting(&cali_config, &handle);
        if (ret == ESP_OK)
        {
            calibrated = true;
        }
    }
#endif

    *out_handle = handle;
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "Calibration success");
    }
    else if (ret == ESP_ERR_NOT_SUPPORTED || !calibrated)
    {
        ESP_LOGW(TAG, "eFuse not burnt, skip software calibration");
    }
    else
    {
        ESP_LOGE(TAG, "Invalid arg or no memory");
    }

    return calibrated;
}

void Battery::_adc_calibration_deinit(adc_cali_handle_t handle)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    ESP_LOGD(TAG, "deregister %s calibration scheme", "Curve Fitting");
    ESP_ERROR_CHECK(adc_cali_delete_scheme_curve_fitting(handle));

#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    ESP_LOGD(TAG, "deregister %s calibration scheme", "Line Fitting");
    ESP_ERROR_CHECK(adc_cali_delete_scheme_line_fitting(handle));
#endif
}

void Battery::init()
{
    esp_err_t ret;

    //-------------ADC1 Init---------------//
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT,
    };

    ret = adc_oneshot_new_unit(&init_config1, &_adc1_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed, ret: %d", ret);
        return;
    }

    //-------------ADC1 Config---------------//
    adc_oneshot_chan_cfg_t config = {
        .atten = ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ret = adc_oneshot_config_channel(_adc1_handle, ADC_CHANNEL, &config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "adc_oneshot_config_channel failed, ret: %d", ret);
        return;
    }

    //-------------ADC1 Calibration Init---------------//
    _do_calibration = _adc_calibration_init(ADC_UNIT, ADC_CHANNEL, ADC_ATTEN, &_adc_cali_handle);

    ESP_LOGI(TAG, "Battery ADC initialized");
}

void Battery::deinit()
{
    if (_adc1_handle != nullptr)
    {
        esp_err_t ret;
        ret = adc_oneshot_del_unit(_adc1_handle);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "adc_oneshot_del_unit failed, ret: %d", ret);
        }
        _adc1_handle = nullptr;
    }

    if (_do_calibration && _adc_cali_handle != nullptr)
    {
        _adc_calibration_deinit(_adc_cali_handle);
        _adc_cali_handle = nullptr;
        _do_calibration = false;
    }

    ESP_LOGI(TAG, "Battery ADC deinitialized");
}

float Battery::get_voltage()
{
    esp_err_t ret;
    ret = adc_oneshot_read(_adc1_handle, ADC_CHANNEL, &_adc_raw);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "adc_oneshot_read failed, ret: %d", ret);
        return 0.0f;
    }

    ESP_LOGD(TAG, "ADC%d Channel[%d] Raw Data: %d", ADC_UNIT, ADC_CHANNEL, _adc_raw);

    if (_do_calibration)
    {
        ret = adc_cali_raw_to_voltage(_adc_cali_handle, _adc_raw, &_voltage_mv);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "adc_cali_raw_to_voltage failed CH0, ret: %d", ret);
            return 0.0f;
        }
        ESP_LOGD(TAG, "ADC%d Channel[%d] Cali Voltage: %d mV", ADC_UNIT, ADC_CHANNEL, _voltage_mv);
        _voltage = static_cast<float>(_voltage_mv * 3.0f / 1000.0f) / MEASUREMENT_OFFSET;
    }

    return _voltage;
}

uint8_t Battery::get_level()
{
    float voltage = get_voltage();
    return get_level(voltage);
}

uint8_t Battery::get_level(float voltage)
{
    // Straight line from 3.30 V (empty) to 4.10 V (full), shared by all boards. This is
    // the formula M5Unified, M5Launcher and Bruce use for these devices, so the number
    // agrees with theirs. A LiPo-shaped curve was tried and read far lower in mid-range
    // (15% where they show 51%).
    static constexpr float EMPTY_V = 3.30f;
    static constexpr float FULL_V = 4.10f;

    if (voltage >= FULL_V)
        return 100;
    if (voltage <= EMPTY_V)
        return 0;
    return static_cast<uint8_t>((voltage - EMPTY_V) * 100.0f / (FULL_V - EMPTY_V));
}
