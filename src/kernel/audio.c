#include "audio.h"
#include "logger.h"
#include "uefi.h"

#pragma intrinsic(__inbyte)
unsigned char __inbyte(unsigned short port);
#pragma intrinsic(__outbyte)
void __outbyte(unsigned short port, unsigned char value);

#define PIT_CHANNEL2_DATA 0x42
#define PIT_COMMAND 0x43
#define SPEAKER_CONTROL 0x61
#define PIT_BASE_FREQUENCY 1193180U

static UINT8 initialized;
static UINT8 hda_controller_present;

int audio_initialize(void)
{
    initialized = 1;
    hda_controller_present = 0;
    return 1;
}

void audio_probe_pci(const ASAS_PCI_REGISTRY *pci_registry)
{
    UINT32 index;

    hda_controller_present = 0;
    if (pci_registry == 0) {
        return;
    }

    for (index = 0; index < pci_registry->count; index++) {
        const ASAS_PCI_DEVICE *device = &pci_registry->devices[index];

        if (device->class_code == 0x04 && device->subclass == 0x03) {
            hda_controller_present = 1;
            return;
        }
    }
}

int audio_pc_speaker_available(void)
{
    return initialized != 0;
}

int audio_hda_controller_present(void)
{
    return hda_controller_present != 0;
}

int audio_beep(void)
{
    UINT32 divisor;
    UINT8 control;

    if (!initialized) {
        return 0;
    }
    divisor = PIT_BASE_FREQUENCY / 880U;
    __outbyte(PIT_COMMAND, 0xB6);
    __outbyte(PIT_CHANNEL2_DATA, (UINT8)(divisor & 0xFFU));
    __outbyte(PIT_CHANNEL2_DATA, (UINT8)(divisor >> 8));
    control = __inbyte(SPEAKER_CONTROL);
    __outbyte(SPEAKER_CONTROL, (UINT8)(control | 0x03U));
    __outbyte(SPEAKER_CONTROL, (UINT8)(control & 0xFCU));
    return 1;
}

int audio_self_test(void)
{
    if (!initialized) {
        return 0;
    }
    logger_write("INFO", "PC speaker audio initialized");
    logger_write("INFO", "PC speaker beep command available");
    if (audio_hda_controller_present()) {
        logger_write("INFO", "HDA audio controller detected");
    } else {
        logger_write("INFO", "HDA audio controller unavailable");
    }
    return 1;
}
