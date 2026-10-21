/* Host simulation of the whole component against a fake I2C register image.
 * Target: rbamp_read_all on a module with more channels than the snapshot
 * struct carries (UI5/UI7) must not write past ch0-2 (host report 2026-10-18:
 * ch3-6 overwrote frequency/topology/channels and four bytes past the struct). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "rbamp.h"

#include "sim_module.h"

/* ---- checks ---- */
static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void run_topology(rbamp_topology_t topo)
{
    struct { rbamp_snapshot_t s; uint32_t guard[8]; } g;
    memset(&g, 0xA5, sizeof g);
    rbamp_handle_t dev = NULL;
    esp_err_t err = rbamp_new_with_topology((i2c_master_bus_handle_t)(uintptr_t)0x10, 0x50, topo, &dev);
    CHECK(err == ESP_OK && dev, "new topo %d: %d", (int)topo, err);
    if (err != ESP_OK) return;
    sim_select_writes = 0; sim_txn = 0;
    err = rbamp_read_all(dev, &g.s);
    CHECK(err == ESP_OK, "read_all topo %d: %d", (int)topo, err);
    for (int i = 0; i < 8; ++i) CHECK(g.guard[i] == 0xA5A5A5A5u, "topo %d guard[%d] = 0x%08x", (int)topo, i, g.guard[i]);
    CHECK(g.s.channels == (uint8_t)topo, "topo %d channels = %u", (int)topo, g.s.channels);
    CHECK(g.s.topology == topo, "topo %d topology = %d", (int)topo, (int)g.s.topology);
    CHECK(g.s.has_voltage_hw == false, "topo %d has_voltage_hw = %d", (int)topo, g.s.has_voltage_hw);
    CHECK(!(g.s.frequency >= 100.0f), "topo %d frequency clobbered by a window value: %g", (int)topo, g.s.frequency);
    for (int ch = 0; ch < RBAMP_SNAPSHOT_CHANNELS; ++ch) {
        CHECK(!(g.s.current[ch] >= 100.0f) && !(g.s.current_peak[ch] >= 100.0f)
              && !(g.s.power[ch] >= 100.0f) && !(g.s.power_factor[ch] >= 100.0f),
              "topo %d ch%d holds a window value (ch3+ leaked in)", (int)topo, ch);
    }
    CHECK(sim_select_writes == 0, "topo %d read_all touched the channel window %u times", (int)topo, sim_select_writes);
    printf("read_all topo %d: %u transactions, guards intact\n", (int)topo, sim_txn);

    /* ch3+ are reachable through the per-channel API, which does use the window */
    if (topo > RBAMP_SNAPSHOT_CHANNELS) {
        rbamp_channel_reading_t r[RBAMP_MAX_CHANNELS + 1];
        memset(r, 0xA5, sizeof r);
        sim_select_writes = 0;
        err = rbamp_read_all_channels(dev, r, RBAMP_MAX_CHANNELS);
        CHECK(err == ESP_OK, "read_all_channels topo %d: %d", (int)topo, err);
        CHECK(sim_select_writes > 0, "topo %d read_all_channels did not use the window for ch3+", (int)topo);
        for (int ch = RBAMP_SNAPSHOT_CHANNELS; ch < (int)topo; ++ch)
            CHECK(isfinite(r[ch].i_rms) && r[ch].i_rms >= 100.0f, "topo %d ch%d i_rms = %g (expected a window value)", (int)topo, ch, r[ch].i_rms);
        uint32_t tail; memcpy(&tail, &r[(int)topo], 4);
        CHECK(tail == 0xA5A5A5A5u, "topo %d read_all_channels wrote past channel %d", (int)topo, (int)topo);
    }
    rbamp_del(dev);
}

int main(void)
{
    sim_reset_image();
    run_topology(RBAMP_TOPOLOGY_SINGLE);
    run_topology(RBAMP_TOPOLOGY_THREE_PHASE);
    run_topology(RBAMP_TOPOLOGY_FIVE_CH);
    run_topology(RBAMP_TOPOLOGY_SEVEN_CH);
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
