#ifndef ASAS_AUDIO_H
#define ASAS_AUDIO_H

#include "pci.h"

int audio_initialize(void);
void audio_probe_pci(const ASAS_PCI_REGISTRY *pci_registry);
int audio_beep(void);
int audio_pc_speaker_available(void);
int audio_hda_controller_present(void);
int audio_self_test(void);

#endif
