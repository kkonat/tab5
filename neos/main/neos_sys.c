/*
 * NeOS system readouts - the firmware half of neos_sys.h.
 *
 * Two small I2C parts are driven from here rather than pulled in as
 * components: the INA226 power monitor and the RX8130 RTC. Both are a handful
 * of register reads and neither has a driver in the component registry that
 * this project would otherwise use, so a driver each would be more code to
 * carry than the registers themselves.
 *
 * Everything is probed once at boot and never again. A part that did not
 * answer then is reported absent for the life of the boot, which is the
 * honest answer on a board where nothing is hot-plugged, and it keeps every
 * reader on the fast path - an app polling the power monitor at 10 Hz must
 * not be paying for a bus scan when the part is missing.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_clk_tree.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sd_protocol_defs.h"
#include "nvs.h"
#include "driver/i2c_master.h"
#include "driver/temperature_sensor.h"
#include "esp_io_expander.h"
#include "esp_io_expander_pi4ioe5v6408.h"

#include "sd_protocol_defs.h"

#include "bsp/m5stack_tab5.h"
#include "bsp/display.h"

#include "neos_net.h"
#include "neos_build.h"

#include "neos_abi.h"

#include "neos_abi.h"
#include "neos_orient.h"
#include "neos_settings.h"
#include "neos_sys.h"

static const char *TAG = "sys";

/* ------------------------------------------------------------------ */
/* The INA226                                                          */
/* ------------------------------------------------------------------ */

#define INA226_REG_CONFIG   0x00
#define INA226_REG_SHUNT    0x01
#define INA226_REG_BUS      0x02
#define INA226_REG_DIE_ID   0xFF
#define INA226_DIE_ID       0x2260   /* top 12 bits identify the part */

/*
 * Averaging 16 samples, 1.1 ms conversion on both channels, shunt and bus
 * measured continuously. Bits 14:12 are fixed at 100 by the part.
 *
 * Continuous matters: readers here never trigger a conversion and never wait
 * for one, they collect whatever the part last finished. At these settings
 * that is a fresh sample about every 35 ms, which is faster than anything
 * looking at a screen can tell.
 */
#define INA226_CONFIG       0x4527

/*
 * The sense resistor, in milliohms.
 *
 * NOT measured off this board - the part reports shunt voltage and has no way
 * to tell us what it is across. Volts are exact regardless; amps and watts
 * scale with this. If a known load ever reads high or low by a clean ratio,
 * this is the number to correct, and shunt_uv in the readout is the raw
 * measurement to correct it against.
 */
#define INA226_SHUNT_MOHM   5

static i2c_master_dev_handle_t s_ina;
static int                     s_ina_addr = -1;

/* ------------------------------------------------------------------ */
/* The RX8130                                                          */
/* ------------------------------------------------------------------ */

#define RX8130_ADDR         0x32
#define RX8130_REG_SEC      0x10   /* SEC MIN HOUR WEEK DAY MONTH YEAR */
#define RX8130_REG_FLAG     0x1D
#define RX8130_REG_CTRL0    0x1E
#define RX8130_FLAG_VLF     0x02   /* the cell went flat; time is not real */
#define RX8130_CTRL0_STOP   0x40   /* freeze the counter while it is written */

static i2c_master_dev_handle_t s_rtc;

/* ------------------------------------------------------------------ */

static temperature_sensor_handle_t s_tsens;

/*
 * What each rail is currently set to.
 *
 * This is the authority, not the expander - see neos_feature() for why. Seeded
 * from the hardware at startup through feat_read_hw(), which is defined with
 * the pin table further down because that is where it belongs.
 */
static bool s_feat_cache[NEOS_FEAT_COUNT];
static bool feat_read_hw(neos_feature_t f);
static bool backlight_apply(int percent, bool save);

static int s_backlight = 100;   /* bsp_display_backlight_on() leaves it here */

/* ------------------------------------------------------------------ */
/* Settings that outlive the app that changed them                     */
/* ------------------------------------------------------------------ */

/*
 * The backlight and the rails are the machine's settings, not an app's. A
 * tablet that came back at full brightness with charging off because that is
 * how some app last left it running would be a tablet nobody could reason
 * about, so they are written to NVS when they change and put back during
 * boot.
 *
 * Backlight writes are deferred. It is on a slider, so a single drag produces
 * tens of values a second and every one of them would be a flash erase cycle;
 * the value that matters is the one the finger stopped on, which is what a
 * one-shot timer restarted on every change ends up committing.
 */
#define SETTINGS_NS      "neos"
#define SETTINGS_BL      "backlight"
#define BL_SETTLE_MS     1500

static nvs_handle_t      s_nvs;
static esp_timer_handle_t s_bl_timer;

/** Key for one rail. Named, not indexed, so renumbering the enum cannot
    silently swap two stored settings. */
static const char *feat_key(neos_feature_t f)
{
    static const char *const KEYS[NEOS_FEAT_COUNT] = {
        "speaker", "camera", "wifi", "usb5v", "charge", "chargeqc", "ext5v", "antenna",
    };
    return KEYS[f];
}

static void setting_save_feature(neos_feature_t f, bool on)
{
    if (!s_nvs) {
        return;
    }
    if (nvs_set_u8(s_nvs, feat_key(f), on ? 1 : 0) == ESP_OK) {
        nvs_commit(s_nvs);
    }
}

static void bl_commit(void *arg)
{
    (void)arg;
    if (!s_nvs) {
        return;
    }
    if (nvs_set_i32(s_nvs, SETTINGS_BL, s_backlight) == ESP_OK) {
        nvs_commit(s_nvs);
        ESP_LOGI(TAG, "backlight %d%% saved", s_backlight);
    }
}

void neos_settings_open(void)
{
    if (s_nvs) {
        return;
    }
    if (nvs_open(SETTINGS_NS, NVS_READWRITE, &s_nvs) != ESP_OK) {
        s_nvs = 0;
        ESP_LOGW(TAG, "no NVS - settings will not survive a reboot");
    }
    const esp_timer_create_args_t args = {
        .callback = bl_commit,
        .name     = "bl_save",
    };
    esp_timer_create(&args, &s_bl_timer);
}

/*
 * The generic accessors, for the settings that do not belong to this file.
 *
 * Committing on every write rather than batching: a setting is changed by a
 * finger, at human speed, and the one thing that must never happen is a value
 * the user watched themselves set being gone after a power cycle. The
 * backlight is the exception and says why in its own comment.
 */
bool neos_setting_u8(const char *key, uint8_t *out)
{
    return s_nvs && out && nvs_get_u8(s_nvs, key, out) == ESP_OK;
}

void neos_setting_set_u8(const char *key, uint8_t v)
{
    if (s_nvs && nvs_set_u8(s_nvs, key, v) == ESP_OK) {
        nvs_commit(s_nvs);
    }
}

bool neos_setting_i32(const char *key, int32_t *out)
{
    return s_nvs && out && nvs_get_i32(s_nvs, key, out) == ESP_OK;
}

void neos_setting_set_i32(const char *key, int32_t v)
{
    if (s_nvs && nvs_set_i32(s_nvs, key, v) == ESP_OK) {
        nvs_commit(s_nvs);
    }
}

bool neos_setting_blob(const char *key, void *out, size_t *len)
{
    if (!s_nvs || !out || !len) {
        return false;
    }
    return nvs_get_blob(s_nvs, key, out, len) == ESP_OK;
}

void neos_setting_set_blob(const char *key, const void *v, size_t len)
{
    if (s_nvs && nvs_set_blob(s_nvs, key, v, len) == ESP_OK) {
        nvs_commit(s_nvs);
    }
}

void neos_setting_erase(const char *key)
{
    if (s_nvs && nvs_erase_key(s_nvs, key) == ESP_OK) {
        nvs_commit(s_nvs);
    }
}

void neos_settings_reset(void)
{
    if (!s_nvs) {
        return;
    }
    nvs_erase_all(s_nvs);
    nvs_commit(s_nvs);
    ESP_LOGW(TAG, "settings cleared - the board's own defaults come back on the next boot");
}

/* ------------------------------------------------------------------ */
/* I2C helpers                                                         */
/* ------------------------------------------------------------------ */

static i2c_master_dev_handle_t attach(uint8_t addr)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = 400000,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &dev) != ESP_OK) {
        return NULL;
    }
    return dev;
}

/** One 16-bit big-endian register. */
static bool reg16(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t *out)
{
    uint8_t rx[2];
    if (i2c_master_transmit_receive(dev, &reg, 1, rx, sizeof(rx), 50) != ESP_OK) {
        return false;
    }
    *out = (uint16_t)((rx[0] << 8) | rx[1]);
    return true;
}

static bool write16(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t val)
{
    const uint8_t tx[3] = { reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFF) };
    return i2c_master_transmit(dev, tx, sizeof(tx), 50) == ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                            */
/* ------------------------------------------------------------------ */

/*
 * The monitor is looked for rather than assumed. INA226 address selection is
 * two pins and four states per pin, so which of 0x40-0x4F it lands on is a
 * board decision - and 0x40 is also where the ES7210 microphone codec sits on
 * this one. The die id makes that unambiguous: only the right part answers
 * 0x226x, so a wrong guess is rejected instead of being read as garbage volts.
 */
static void ina226_probe(void)
{
    for (uint8_t addr = 0x40; addr <= 0x4F; addr++) {
        i2c_master_dev_handle_t dev = attach(addr);
        if (!dev) {
            continue;
        }
        uint16_t id = 0;
        if (reg16(dev, INA226_REG_DIE_ID, &id) && (id & 0xFFF0) == INA226_DIE_ID) {
            if (write16(dev, INA226_REG_CONFIG, INA226_CONFIG)) {
                s_ina = dev;
                s_ina_addr = addr;

                /* One sample in the boot log. The shunt resistance is a
                   constant we cannot read off the part, so the first thing
                   anyone will want when the amps look wrong is the raw
                   microvolts they were derived from. */
                neos_power_t p = {0};
                if (neos_power_read(&p)) {
                    ESP_LOGI(TAG, "INA226 at 0x%02X: %ld mV, %ld uV shunt, "
                                  "%ld mA over %d mOhm",
                             addr, (long)p.bus_mv, (long)p.shunt_uv,
                             (long)p.current_ma, INA226_SHUNT_MOHM);
                } else {
                    ESP_LOGW(TAG, "INA226 at 0x%02X but it will not read", addr);
                }
                return;
            }
        }
        i2c_master_bus_rm_device(dev);
    }
    ESP_LOGW(TAG, "no INA226 on the bus - power readings unavailable");
}

static void rx8130_probe(void)
{
    if (i2c_master_probe(bsp_i2c_get_handle(), RX8130_ADDR, 50) != ESP_OK) {
        ESP_LOGW(TAG, "no RTC at 0x%02X", RX8130_ADDR);
        return;
    }
    s_rtc = attach(RX8130_ADDR);
    if (s_rtc) {
        ESP_LOGI(TAG, "RX8130 RTC at 0x%02X", RX8130_ADDR);
    }
}

static void tsens_start(void)
{
    const temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&cfg, &s_tsens) != ESP_OK) {
        s_tsens = NULL;
        return;
    }
    if (temperature_sensor_enable(s_tsens) != ESP_OK) {
        temperature_sensor_uninstall(s_tsens);
        s_tsens = NULL;
    }
}

void neos_sys_init(void)
{
    ina226_probe();
    rx8130_probe();
    tsens_start();

    /*
     * Seed the cache from the hardware before anything is restored, so a rail
     * with nothing stored against it keeps reporting whatever the BSP left it
     * at during bring-up rather than a guess.
     */
    for (int i = 0; i < NEOS_FEAT_COUNT; i++) {
        s_feat_cache[i] = feat_read_hw((neos_feature_t)i);
    }

    neos_settings_open();
    if (!s_nvs) {
        return;
    }

    /*
     * Only keys that are actually there are applied. A first boot, or a boot
     * after neos_settings_reset(), must leave the board exactly as its own
     * bring-up left it - restoring a default we invented would be this code
     * deciding how the tablet powers up, which is not its call.
     */
    int32_t bl = 0;
    if (nvs_get_i32(s_nvs, SETTINGS_BL, &bl) == ESP_OK) {
        /* Applied without arming the save timer. Writing back the value we
           have just read would be a flash erase cycle on every single boot,
           to store what is already stored. */
        backlight_apply((int)bl, false);
        ESP_LOGI(TAG, "backlight restored to %d%%", s_backlight);
    }

    for (int i = 0; i < NEOS_FEAT_COUNT; i++) {
        uint8_t on = 0;
        if (nvs_get_u8(s_nvs, feat_key((neos_feature_t)i), &on) == ESP_OK) {
            neos_feature_set((neos_feature_t)i, on != 0);
        }
    }
}

/* ------------------------------------------------------------------ */
/* The machine                                                         */
/* ------------------------------------------------------------------ */

const char *neos_chip(void)
{
    static char buf[32];
    if (!buf[0]) {
        esp_chip_info_t info;
        esp_chip_info(&info);
        const unsigned rev = (unsigned)info.revision;
        snprintf(buf, sizeof(buf), "ESP32-P4 rev v%u.%u", rev / 100, rev % 100);
    }
    return buf;
}

const char *neos_mac(void)
{
    static char buf[18];
    if (!buf[0]) {
        uint8_t m[6] = {0};
        esp_base_mac_addr_get(m);
        snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
                 m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    return buf;
}

const char *neos_reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_EXT:      return "reset pin";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_INT_WDT:  return "int watchdog";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB:      return "USB";
    default:               return "other";
    }
}

/*
 * Which build this is.
 *
 * The same counter the system bar shows, so a screenful of details and the
 * badge in the corner can never disagree about what is running.
 */
const char *neos_build(void)
{
    static char buf[16];
    if (!buf[0]) {
        snprintf(buf, sizeof(buf), "v0.%d", NEOS_BUILD);
    }
    return buf;
}

const char *neos_build_date(void)
{
    return __DATE__ " " __TIME__;
}

uint32_t neos_abi(void)
{
    return ((uint32_t)NEOS_ABI_MAJOR << 16) | (uint32_t)NEOS_ABI_MINOR;
}

const char *neos_idf_version(void)
{
    return IDF_VER;
}

uint16_t neos_cpu_mhz(void)
{
    uint32_t hz = 0;
    if (esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU,
                                     ESP_CLK_TREE_SRC_FREQ_PRECISION_APPROX,
                                     &hz) != ESP_OK) {
        return 0;
    }
    return (uint16_t)(hz / 1000000);
}

uint8_t neos_cores(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    return info.cores;
}

uint64_t neos_uptime_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

uint32_t neos_uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

uint64_t neos_uptime_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

uint32_t neos_heap_free(void)   { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }
uint32_t neos_heap_total(void)  { return heap_caps_get_total_size(MALLOC_CAP_INTERNAL); }
uint32_t neos_psram_free(void)  { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }
uint32_t neos_psram_total(void) { return heap_caps_get_total_size(MALLOC_CAP_SPIRAM); }

/*
 * Internal RAM, for the small number of bytes an app touches constantly.
 *
 * Aligned to a cache line because the two reasons to want this memory arrive
 * together: it is fast for the CPU, and it is what the PPA can be pointed at
 * without ngl having to copy it somewhere acceptable first.
 *
 * No fall back to PSRAM when the pool is short. malloc() already does that and
 * is the right call for almost everything; the only reason to be here is that
 * PSRAM would be the wrong answer, and quietly giving it anyway would turn a
 * failure an app could report into a frame rate nobody can explain.
 */
void *neos_alloc_fast(size_t n)
{
    if (!n) {
        return NULL;
    }
    return heap_caps_aligned_alloc(64, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

/* ------------------------------------------------------------------ */
/* Sensors                                                             */
/* ------------------------------------------------------------------ */

/*
 * g and dps arrive as floats from the BMI270 driver and leave as integers.
 * Rounded, not truncated: a value sitting on a boundary would otherwise
 * flicker down by one every other frame.
 */
static int16_t round16(float v)
{
    return (int16_t)(v < 0 ? v - 0.5f : v + 0.5f);
}

bool neos_imu_accel_mg(int16_t *x, int16_t *y, int16_t *z)
{
    float ax = 0, ay = 0, az = 0;
    if (neos_orient_read(&ax, &ay, &az) != ESP_OK) {
        return false;
    }
    if (x) { *x = round16(ax * 1000.0f); }
    if (y) { *y = round16(ay * 1000.0f); }
    if (z) { *z = round16(az * 1000.0f); }
    return true;
}

bool neos_imu_gyro_dps(int16_t *x, int16_t *y, int16_t *z)
{
    float gx = 0, gy = 0, gz = 0;
    if (neos_orient_read_gyro(&gx, &gy, &gz) != ESP_OK) {
        return false;
    }
    /* Already degrees per second; only the rounding applies. */
    if (x) { *x = round16(gx); }
    if (y) { *y = round16(gy); }
    if (z) { *z = round16(gz); }
    return true;
}

int16_t neos_die_temp_c10(void)
{
    float c = 0;
    if (!s_tsens || temperature_sensor_get_celsius(s_tsens, &c) != ESP_OK) {
        return INT16_MIN;
    }
    return round16(c * 10.0f);
}

static uint8_t bcd(uint8_t v)
{
    return (uint8_t)((v >> 4) * 10 + (v & 0x0F));
}

bool neos_rtc_read(neos_rtc_t *out)
{
    if (!s_rtc || !out) {
        return false;
    }

    const uint8_t reg = RX8130_REG_SEC;
    uint8_t rx[7];
    if (i2c_master_transmit_receive(s_rtc, &reg, 1, rx, sizeof(rx), 50) != ESP_OK) {
        return false;
    }

    neos_rtc_t t = {0};
    t.sec   = bcd(rx[0] & 0x7F);
    t.min   = bcd(rx[1] & 0x7F);
    t.hour  = bcd(rx[2] & 0x3F);
    t.wday  = 0;
    t.day   = bcd(rx[4] & 0x3F);
    t.month = bcd(rx[5] & 0x1F);
    t.year  = (int16_t)(2000 + bcd(rx[6]));

    /* WEEK is a one-hot bitmap, not a count. Anything else means the register
       is not holding a real weekday, in which case Sunday is as good a lie as
       any and the rest of the reading is still checked below. */
    for (int i = 0; i < 7; i++) {
        if (rx[3] & (1 << i)) {
            t.wday = (uint8_t)i;
            break;
        }
    }

    /* A cell that has never been set answers, it just answers nonsense. The
       caller asked for a time, so give it one or say there is none. */
    if (t.sec > 59 || t.min > 59 || t.hour > 23 ||
        t.day < 1 || t.day > 31 || t.month < 1 || t.month > 12) {
        return false;
    }

    *out = t;
    return true;
}

/*
 * Writing the clock.
 *
 * The counter is stopped first and started again afterwards, because the
 * seconds register can roll over between the write of the seconds and the
 * write of the minutes - which is how a clock set at 09:59:59 ends up an hour
 * out roughly once every three thousand times, and how it stays out until
 * somebody sets it again.
 *
 * The low-voltage flag is cleared on the way past. It is the part's own record
 * that its backup cell went flat and the time in it is meaningless; leaving it
 * set after deliberately writing a good time would be keeping a warning about
 * data that is no longer there.
 */
static bool rtc_reg8(uint8_t reg, uint8_t *out)
{
    return s_rtc && i2c_master_transmit_receive(s_rtc, &reg, 1, out, 1, 50) == ESP_OK;
}

static bool rtc_write8(uint8_t reg, uint8_t val)
{
    const uint8_t tx[2] = { reg, val };
    return s_rtc && i2c_master_transmit(s_rtc, tx, sizeof(tx), 50) == ESP_OK;
}

static uint8_t to_bcd(uint8_t v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

bool neos_rtc_set(const neos_rtc_t *t)
{
    if (!s_rtc || !t) {
        return false;
    }
    /* A caller with a bad time is a caller with a bug, and a clock chip is
       exactly the wrong place to find out about it six months later. */
    if (t->year < 2000 || t->year > 2099 ||
        t->month < 1 || t->month > 12 || t->day < 1 || t->day > 31 ||
        t->hour > 23 || t->min > 59 || t->sec > 59 || t->wday > 6) {
        ESP_LOGW(TAG, "refusing to set the RTC to %04d-%02u-%02u %02u:%02u:%02u",
                 t->year, t->month, t->day, t->hour, t->min, t->sec);
        return false;
    }

    uint8_t ctrl0 = 0;
    if (!rtc_reg8(RX8130_REG_CTRL0, &ctrl0)) {
        return false;
    }
    if (!rtc_write8(RX8130_REG_CTRL0, (uint8_t)(ctrl0 | RX8130_CTRL0_STOP))) {
        return false;
    }

    /* WEEK is a one-hot bitmap, not a count - the same asymmetry neos_rtc_read
       has to undo on the way back. */
    const uint8_t tx[8] = {
        RX8130_REG_SEC,
        to_bcd(t->sec), to_bcd(t->min), to_bcd(t->hour),
        (uint8_t)(1u << t->wday),
        to_bcd(t->day), to_bcd(t->month), to_bcd((uint8_t)(t->year - 2000)),
    };
    const bool wrote = i2c_master_transmit(s_rtc, tx, sizeof(tx), 50) == ESP_OK;

    uint8_t flag = 0;
    if (rtc_reg8(RX8130_REG_FLAG, &flag)) {
        rtc_write8(RX8130_REG_FLAG, (uint8_t)(flag & ~RX8130_FLAG_VLF));
    }
    rtc_write8(RX8130_REG_CTRL0, (uint8_t)(ctrl0 & ~RX8130_CTRL0_STOP));

    if (wrote) {
        ESP_LOGI(TAG, "RTC set to %04d-%02u-%02u %02u:%02u:%02u",
                 t->year, t->month, t->day, t->hour, t->min, t->sec);
    }
    return wrote;
}

/* ------------------------------------------------------------------ */
/* Power                                                               */
/* ------------------------------------------------------------------ */

bool neos_power_read(neos_power_t *out)
{
    if (!s_ina || !out) {
        return false;
    }

    uint16_t bus = 0, shunt = 0;
    if (!reg16(s_ina, INA226_REG_BUS, &bus) ||
        !reg16(s_ina, INA226_REG_SHUNT, &shunt)) {
        return false;
    }

    neos_power_t p;
    /* 1.25 mV per bus LSB, unsigned. */
    p.bus_mv = (int32_t)bus * 5 / 4;
    /* 2.5 uV per shunt LSB, two's complement - current can flow either way
       through the sense resistor and the sign is the difference between
       charging and discharging. */
    p.shunt_uv = (int32_t)(int16_t)shunt * 5 / 2;
    /* microvolts over milliohms is milliamps, with no scaling left over. */
    p.current_ma = p.shunt_uv / INA226_SHUNT_MOHM;
    p.power_mw   = (int32_t)((int64_t)p.bus_mv * p.current_ma / 1000);

    *out = p;
    return true;
}

int neos_power_monitor_addr(void)
{
    return s_ina_addr;
}

/* ------------------------------------------------------------------ */
/* Backlight                                                           */
/* ------------------------------------------------------------------ */

/*
 * The floor. Below about this the panel is dark enough that a slider is no
 * longer findable, and the only way back would be a reset.
 */
#define BACKLIGHT_MIN 5

int neos_backlight(void)
{
    return s_backlight;
}

static bool backlight_apply(int percent, bool save)
{
    if (percent < BACKLIGHT_MIN) { percent = BACKLIGHT_MIN; }
    if (percent > 100)           { percent = 100; }
    if (bsp_display_brightness_set(percent) != ESP_OK) {
        return false;
    }
    s_backlight = percent;

    /* Restarted, not started: while a finger is on the slider the commit keeps
       being pushed out ahead of it, and lands once, on the value it settled
       on. */
    if (save && s_bl_timer) {
        esp_timer_stop(s_bl_timer);
        esp_timer_start_once(s_bl_timer, BL_SETTLE_MS * 1000);
    }
    return true;
}

bool neos_backlight_set(int percent)
{
    return backlight_apply(percent, true);
}

/* ------------------------------------------------------------------ */
/* Switchable rails                                                    */
/* ------------------------------------------------------------------ */

/*
 * Which expander, which pin, and which way round.
 *
 * Espressif's BSP names four of these and switches them through
 * bsp_feature_enable(). The other four - charging, fast charging, the
 * external 5 V rail and the antenna select - are not in that BSP at all, so
 * the pins come from M5's own board support for this tablet, cross-checked
 * against the four Espressif does name: both agree that Wi-Fi power is bit 0
 * and USB-A 5 V bit 3 of the high-address expander, which is what makes the
 * rest of M5's map trustworthy here.
 *
 * Fast charge is the one line that is active low. That is a property of the
 * board, not of this code, and it is the reason every rail goes through one
 * table with a polarity column rather than through the BSP call: a toggle
 * that reads back inverted is worse than no toggle.
 */
#define EXP_LOW   0     /* 0x43 - M5 calls it pi4ioe1 */
#define EXP_HIGH  1     /* 0x44 - pi4ioe2 */

static const struct {
    uint8_t     expander;
    uint8_t     bit;
    bool        active_low;
    const char *name;
} FEATS[NEOS_FEAT_COUNT] = {
    [NEOS_FEAT_SPEAKER]   = { EXP_LOW,  1, false, "Speaker" },
    [NEOS_FEAT_CAMERA]    = { EXP_LOW,  6, false, "Camera" },
    [NEOS_FEAT_WIFI]      = { EXP_HIGH, 0, false, "Wi-Fi C6" },
    [NEOS_FEAT_USB_5V]    = { EXP_HIGH, 3, false, "USB-A 5V" },
    [NEOS_FEAT_CHARGE]    = { EXP_HIGH, 7, false, "Charging" },
    [NEOS_FEAT_CHARGE_QC] = { EXP_HIGH, 5, true,  "Fast charge" },
    [NEOS_FEAT_EXT_5V]    = { EXP_LOW,  2, false, "Ext 5V" },
    [NEOS_FEAT_ANTENNA]   = { EXP_LOW,  0, false, "Ext antenna" },
};

/* The Espressif BSP pin macros are masks, and this is where a mistyped bit
   number would otherwise become a silently different rail. */
_Static_assert((1U << 1) == BSP_SPEAKER_EN, "speaker pin moved in the BSP");
_Static_assert((1U << 6) == BSP_CAMERA_EN,  "camera pin moved in the BSP");
_Static_assert((1U << 0) == BSP_WIFI_EN,    "wifi pin moved in the BSP");
_Static_assert((1U << 3) == BSP_USB_EN,     "usb pin moved in the BSP");

static bool feat_valid(neos_feature_t f)
{
    return (int)f >= 0 && (int)f < NEOS_FEAT_COUNT;
}

static esp_io_expander_handle_t feat_dev(neos_feature_t f)
{
    return FEATS[f].expander == EXP_LOW ? bsp_io_expander_init()
                                        : bsp_io_expander1_init();
}

const char *neos_feature_name(neos_feature_t f)
{
    return feat_valid(f) ? FEATS[f].name : "";
}

/*
 * Ask the hardware. Only used to seed the cache - see neos_feature().
 *
 * The output register, not the input one. On the PI4IOE5V6408 the input
 * register reports what the pad is being driven to from outside, and a pin
 * this code owns is driven from inside - so an output sitting at a solid high
 * still reads back 0 there. Seeding from it made every rail come up reading
 * "off" no matter what it was actually doing, which is worse than not asking
 * at all: four switches on the power page that were off because the question
 * was wrong, not because the rail was.
 *
 * A pin still in high impedance has never been driven by anyone. Its output
 * bit is 0, and the board's pull-downs mean off is also what it physically is,
 * so no special case is needed for it.
 */
#define PI4IO_REG_OUT 0x05

static i2c_master_dev_handle_t s_exp[2];

static bool feat_read_hw(neos_feature_t f)
{
    const int e = FEATS[f].expander;
    if (!s_exp[e]) {
        s_exp[e] = attach(e == EXP_LOW ? BSP_IO_EXPANDER_ADDRESS
                                       : BSP_IO_EXPANDER_ADDRESS_1);
        if (!s_exp[e]) {
            return false;
        }
    }

    const uint8_t reg = PI4IO_REG_OUT;
    uint8_t v = 0;
    if (i2c_master_transmit_receive(s_exp[e], &reg, 1, &v, 1, 50) != ESP_OK) {
        return false;
    }
    const bool high = (v >> FEATS[f].bit) & 1u;
    return FEATS[f].active_low ? !high : high;
}

/*
 * Answered from memory, not from the bus.
 *
 * This used to read the expander on every call, which is the more obviously
 * correct thing right up until you notice who calls it: a UI drawing a page of
 * switches asks every one of them on every repaint. That turned a getter into
 * four I2C transactions per frame, on the one bus the touch controller is also
 * polled from - and an expander read that times out holds the bus lock for
 * 50 ms while the touch task is trying to use it. The symptom is a system bar
 * that needs a long press instead of a tap, and only on the screen that
 * happens to show four rails.
 *
 * The cache is accurate for everything that goes through neos_feature_set(),
 * which is every deliberate change: the pins are outputs, so what was last
 * written is what they are. The gap is the BSP - bsp_camera_start() and the
 * audio bring-up flip their own rails without telling us, so those two can
 * read stale until something sets them through here. That is worth knowing
 * and not worth a bus transaction per repaint to close; the hardware is read
 * once at startup, which is what the BSP has already finished doing by then.
 */
bool neos_feature(neos_feature_t f)
{
    return feat_valid(f) ? s_feat_cache[f] : false;
}

/*
 * Set the pin, and nothing else.
 *
 * Used by the network service, which owns the sequencing the radio rail needs
 * and must not be sent back through neos_feature_set() below.
 */
bool neos_feature_set_raw(neos_feature_t f, bool on)
{
    if (!feat_valid(f)) {
        return false;
    }

    esp_io_expander_handle_t dev = feat_dev(f);
    if (!dev) {
        return false;
    }

    const uint32_t pin = 1U << FEATS[f].bit;
    const uint8_t level = (FEATS[f].active_low ? !on : on) ? 1 : 0;

    esp_err_t err = esp_io_expander_set_dir(dev, pin, IO_EXPANDER_OUTPUT);
    err |= esp_io_expander_set_level(dev, pin, level);
    err |= esp_io_expander_set_output_mode(dev, pin, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: expander did not take it", FEATS[f].name);
        return false;
    }

    s_feat_cache[f] = on;
    setting_save_feature(f, on);
    ESP_LOGI(TAG, "%s %s", FEATS[f].name, on ? "on" : "off");
    return true;
}

/*
 * The rail an app is allowed to ask about, with the one rail that is not
 * really a rail routed away.
 *
 * The C6 sits at the far end of an SDIO bus with ESP-Hosted pumping it, so its
 * enable line stopped being a switch the moment there was a driver on top of
 * it: cutting power under a live transport makes ESP-Hosted declare the link
 * unrecoverable and reboot the tablet. The network service knows how to stop
 * the stack first, so "switch the radio" means "ask it to", and the toggle
 * catches up when it has actually happened rather than when it was asked for.
 */
bool neos_feature_set(neos_feature_t f, bool on)
{
    if (f == NEOS_FEAT_WIFI) {
        neos_net_power(on);
        return true;
    }
    return neos_feature_set_raw(f, on);
}

/* ------------------------------------------------------------------ */
/* The card                                                            */
/* ------------------------------------------------------------------ */

bool neos_sd_mounted(void)
{
    return bsp_sdcard_get_handle() != NULL;
}

uint64_t neos_sd_bytes(void)
{
    const sdmmc_card_t *c = bsp_sdcard_get_handle();
    if (!c) {
        return 0;
    }
    return (uint64_t)c->csd.capacity * c->csd.sector_size;
}

/*
 * What is left, asked of the filesystem rather than the card.
 *
 * Walks the FAT to count free clusters, so it costs real time on a large card
 * and is not something to put on a tick. The card's own capacity above is free
 * to read; this one is not, and the caller is expected to know the difference.
 */
uint64_t neos_sd_free_bytes(void)
{
    const sdmmc_card_t *c = bsp_sdcard_get_handle();
    if (!c) {
        return 0;
    }
    FATFS *fs = NULL;
    DWORD free_clusters = 0;
    if (f_getfree(BSP_SD_MOUNT_POINT, &free_clusters, &fs) != FR_OK || !fs) {
        return 0;
    }
    return (uint64_t)free_clusters * fs->csize * c->csd.sector_size;
}

const char *neos_sd_type(void)
{
    const sdmmc_card_t *c = bsp_sdcard_get_handle();
    if (!c) {
        return "";
    }
    if (c->is_sdio) {
        return "SDIO";
    }
    if (c->is_mmc) {
        return "MMC";
    }
    /* The capacity bit in the OCR is the whole difference between the two
       addressing schemes, and it is what every other tool calls the card. */
    return (c->ocr & SD_OCR_SDHC_CAP) ? "SDHC/SDXC" : "SDSC";
}

uint32_t neos_sd_speed_khz(void)
{
    const sdmmc_card_t *c = bsp_sdcard_get_handle();
    return c ? (uint32_t)c->max_freq_khz : 0;
}

int neos_sd_bus_width(void)
{
    const sdmmc_card_t *c = bsp_sdcard_get_handle();
    if (!c) {
        return 0;
    }
    return (c->host.flags & SDMMC_HOST_FLAG_4BIT) ? 4 : 1;
}

const char *neos_sd_mount(void)
{
    return BSP_SD_MOUNT_POINT;
}

const char *neos_sd_name(void)
{
    const sdmmc_card_t *c = bsp_sdcard_get_handle();
    return c ? c->cid.name : "";
}


/* ------------------------------------------------------------------ */
/* The I2C bus                                                         */
/* ------------------------------------------------------------------ */

int neos_i2c_scan(uint8_t *addrs, int max)
{
    int found = 0;
    /* 0x00-0x07 and 0x78-0x7F are reserved by the spec; probing them tells
       you nothing and a couple of them are broadcast. */
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        if (i2c_master_probe(bsp_i2c_get_handle(), a, 10) != ESP_OK) {
            continue;
        }
        if (addrs && found < max) {
            addrs[found] = a;
        }
        found++;
    }
    return found;
}

/*
 * The parts this board is known to carry, from docs/specs_fingerprint.md and
 * the BSP headers. An address that is not on the list still gets reported by
 * the scan - it just gets reported without a name, which is exactly what you
 * want when something unexpected has appeared on the bus.
 */
const char *neos_i2c_name(uint8_t addr)
{
    /* Checked first: the monitor was found by its die id, so where it sits
       beats any guess from a table - and on this board its range overlaps the
       two io expanders. */
    if ((int)addr == s_ina_addr) {
        return "INA226 power";
    }
    switch (addr) {
    case 0x10: case 0x11: return "ES8388 audio out";
    case 0x1A:            return "ST7123 touch";
    case 0x32:            return "RX8130 RTC";
    case 0x40:            return "ES7210 mics";
    case 0x43: case 0x44: return "PI4IOE5V6408 io";
    case 0x5D:            return "GT911 touch";
    case 0x68: case 0x69: return "BMI270 IMU";
    default:              return "";
    }
}
