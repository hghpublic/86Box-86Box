/*
 * 86Box     A hypervisor and IBM PC system emulator that specializes in
 *           running old operating systems and software designed for IBM
 *           PC systems and compatibles from 1981 through fairly recent
 *           system designs based on the PCI bus.
 *
 *           This file is part of the 86Box distribution.
 *
 *           ESS AudioDrive emulation.
 *
 *
 *
 * Authors:  Sarah Walker, <https://pcem-emulator.co.uk/>
 *           Miran Grca, <mgrca8@gmail.com>
 *           TheCollector1995, <mariogplayer@gmail.com>
 *           Cacodemon345,
 *           Kagamiin~, <kagamiin@riseup.net>
 *
 *           Copyright 2008-2020 Sarah Walker.
 *           Copyright 2016-2020 Miran Grca.
 *           Copyright 2024 Cacodemon345
 *           Copyright 2024 Kagamiin~
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <wchar.h>
#define HAVE_STDARG_H

#include <86box/86box.h>
#include <86box/device.h>
#include <86box/filters.h>
#include <86box/gameport.h>
#include <86box/hdc.h>
#include <86box/isapnp.h>
#include <86box/hdc_ide.h>
#include <86box/io.h>
#include <86box/mca.h>
#include <86box/mem.h>
#include <86box/midi.h>
#include <86box/pic.h>
#include <86box/rom.h>
#include <86box/sound.h>
#include <86box/timer.h>
#include <86box/snd_sb.h>
#include <86box/plat_unused.h>

#    define sb_log(fmt, ...)

// clang-format off
static const double sb_att_4dbstep_3bits[] = {
      164.0,  2067.0,  3276.0,  5193.0,  8230.0, 13045.0, 20675.0, 32767.0
};

static const double sb_att_7dbstep_2bits[] = {
      164.0,  6537.0, 14637.0, 32767.0
};

/* Attenuation table for 4-bit microphone volume.
 * The last step is a jump to -48 dB. */
static const double sb_att_1p4dbstep_4bits[] = {
      164.0,  3431.0,  4031.0,  4736.0,  5565.0,  6537.0,  7681.0,  9025.0,
    10603.0, 12458.0, 14637.0, 17196.0, 20204.0, 23738.0, 27889.0, 32767.0
};

/* Attenuation table for 4-bit mixer volume.
 * The last step is a jump to -48 dB. */
static const double sb_att_2dbstep_4bits[] = {
      164.0,  1304.0,  1641.0,  2067.0,  2602.0,  3276.0,  4125.0,  5192.0,
     6537.0,  8230.0, 10362.0, 13044.0, 16422.0, 20674.0, 26027.0, 32767.0
};
// clang-format on

/* SB PRO */
typedef struct ess_mixer_t {
    double master_l;
    double master_r;
    double voice_l;
    double voice_r;
    double fm_l;
    double fm_r;
    double cd_l;
    double cd_r;
    double line_l;
    double line_r;
    double mic_l;
    double mic_r;
    double auxb_l;
    double auxb_r;
    /*see sb_ct1745_mixer for values for input selector*/
    int32_t input_selector;

    int input_filter;
    int in_filter_freq;
    int output_filter;

    int stereo;
    int stereo_isleft;

    uint8_t index;
    uint8_t regs[256];

    uint8_t ess_id_str[4];
    uint8_t ess_id_str_pos;
} ess_mixer_t;

typedef struct ess_t {
    uint8_t     mixer_enabled;
    fm_drv_t    opl;
    sb_dsp_t    dsp;
    ess_mixer_t mixer_sbpro;

    mpu_t *mpu;
    void  *gameport;

    uint16_t gameport_addr;

    void *opl_mixer;
    void (*opl_mix)(void *, double *, double *);
} ess_t;

static double
ess_mixer_get_vol_4bit(uint8_t vol)
{
    return sb_att_2dbstep_4bits[vol & 0x0F] / 32767.0;
}

void
ess_mixer_write(uint16_t addr, uint8_t val, void *priv)
{
    ess_t       *ess   = (ess_t *) priv;
    ess_mixer_t *mixer = &ess->mixer_sbpro;

    if (!(addr & 1)) {
        mixer->index      = val;
        mixer->regs[0x01] = val;
        if (val == 0x40) {
            mixer->ess_id_str_pos = 0;
        }
    } else {
        if (mixer->index == 0) {
            /* Reset */
            mixer->regs[0x0a] = mixer->regs[0x0c] = 0x00;
            mixer->regs[0x0e]                     = 0x00;
            /* Changed default from -11dB to 0dB */
            mixer->regs[0x04] = mixer->regs[0x22] = 0xee;
            mixer->regs[0x26] = mixer->regs[0x28] = 0xee;
            mixer->regs[0x2e]                     = 0x00;

            /* Initialize ESS regs. */
            mixer->regs[0x14] = mixer->regs[0x32] = 0x88;
            mixer->regs[0x36]                     = 0x88;
            mixer->regs[0x38]                     = 0x88;
            mixer->regs[0x3a]                     = 0x00;
            mixer->regs[0x3e]                     = 0x00;

            sb_dsp_set_stereo(&ess->dsp, mixer->regs[0x0e] & 2);
        } else {
            mixer->regs[mixer->index] = val;

            switch (mixer->index) {
                /* Compatibility: chain registers 0x02 and 0x22 as well as 0x06 and 0x26 */
                case 0x02:
                case 0x06:
                case 0x08:
                    mixer->regs[mixer->index + 0x20] = ((val & 0xe) << 4) | (val & 0xe);
                    break;

                case 0x0A:
                    {
                        uint8_t mic_vol_2bit = (mixer->regs[0x0a] >> 1) & 0x3;
                        mixer->mic_l = mixer->mic_r = sb_att_7dbstep_2bits[mic_vol_2bit] / 32768.0;
                        mixer->regs[0x1A]           = mic_vol_2bit | (mic_vol_2bit << 2) | (mic_vol_2bit << 4) | (mic_vol_2bit << 6);
                        break;
                    }

                case 0x0C:
                    switch (mixer->regs[0x0C] & 6) {
                        case 2:
                            mixer->input_selector = INPUT_CD_L | INPUT_CD_R;
                            break;
                        case 6:
                            mixer->input_selector = INPUT_LINE_L | INPUT_LINE_R;
                            break;
                        default:
                            mixer->input_selector = INPUT_MIC;
                            break;
                    }
                    break;

                case 0x14:
                    mixer->regs[0x4] = val & 0xee;
                    break;

                case 0x1A:
                    mixer->mic_l = sb_att_1p4dbstep_4bits[(mixer->regs[0x1A] >> 4) & 0xF];
                    mixer->mic_r = sb_att_1p4dbstep_4bits[mixer->regs[0x1A] & 0xF];
                    break;

                case 0x1C:
                    if ((mixer->regs[0x1C] & 0x07) == 0x07) {
                        mixer->input_selector = INPUT_MIXER_L | INPUT_MIXER_R;
                    } else if ((mixer->regs[0x1C] & 0x07) == 0x06) {
                        mixer->input_selector = INPUT_LINE_L | INPUT_LINE_R;
                    } else if ((mixer->regs[0x1C] & 0x06) == 0x02) {
                        mixer->input_selector = INPUT_CD_L | INPUT_CD_R;
                    } else if ((mixer->regs[0x1C] & 0x02) == 0) {
                        mixer->input_selector = INPUT_MIC;
                    }
                    break;

                case 0x22:
                case 0x26:
                case 0x28:
                case 0x2E:
                    mixer->regs[mixer->index - 0x20] = (val & 0xe);
                    mixer->regs[mixer->index + 0x10] = val;
                    break;

                /* More compatibility:
                   SoundBlaster Pro selects register 020h for 030h, 022h for 032h,
                   026h for 036h, and 028h for 038h. */
                case 0x30:
                    mixer->regs[mixer->index - 0x10] = (val & 0xee);
                    break;
                case 0x32:
                case 0x36:
                case 0x38:
                case 0x3e:
                    mixer->regs[mixer->index - 0x10] = (val & 0xee);
                    break;

                case 0x3a:
                    break;

                case 0x00:
                case 0x04:
                    break;

                case 0x0e:
                    break;

                case 0x64:
                    mixer->regs[mixer->index] &= ~0x8;
                    break;

                case 0x40:
                    {
                        uint16_t mpu401_base_addr = 0x300 | ((mixer->regs[0x40] << 1) & 0x30);
                        gameport_remap(ess->gameport, !(mixer->regs[0x40] & 0x2) ? 0x00 : 0x200);

                        if (0) {
                            /* Not on ES1688. */
                            io_removehandler(0x0388, 0x0004,
                                             ess->opl.read, NULL, NULL,
                                             ess->opl.write, NULL, NULL,
                                             ess->opl.priv);
                            if ((mixer->regs[0x40] & 0x1) != 0) {
                                io_sethandler(0x0388, 0x0004,
                                              ess->opl.read, NULL, NULL,
                                              ess->opl.write, NULL, NULL,
                                              ess->opl.priv);
                            }
                        }
                        switch ((mixer->regs[0x40] >> 5) & 0x7) {
                            case 0:
                                mpu401_change_addr(ess->mpu, 0x00);
                                mpu401_setirq(ess->mpu, -1);
                                break;
                            case 1:
                                mpu401_change_addr(ess->mpu, mpu401_base_addr);
                                mpu401_setirq(ess->mpu, -1);
                                break;
                            case 2:
                                mpu401_change_addr(ess->mpu, mpu401_base_addr);
                                mpu401_setirq(ess->mpu, ess->dsp.sb_irqnum);
                                break;
                            case 3:
                                mpu401_change_addr(ess->mpu, mpu401_base_addr);
                                mpu401_setirq(ess->mpu, 11);
                                break;
                            case 4:
                                mpu401_change_addr(ess->mpu, mpu401_base_addr);
                                mpu401_setirq(ess->mpu, 9);
                                break;
                            case 5:
                                mpu401_change_addr(ess->mpu, mpu401_base_addr);
                                mpu401_setirq(ess->mpu, 5);
                                break;
                            case 6:
                                mpu401_change_addr(ess->mpu, mpu401_base_addr);
                                mpu401_setirq(ess->mpu, 7);
                                break;
                            case 7:
                                mpu401_change_addr(ess->mpu, mpu401_base_addr);
                                mpu401_setirq(ess->mpu, 10);
                                break;
                        }
                        break;
                    }

                default:
                    sb_log("ess: Unknown mixer register WRITE: %02X\t%02X\n", mixer->index, mixer->regs[mixer->index]);
                    break;
            }
        }

        mixer->voice_l  = ess_mixer_get_vol_4bit(mixer->regs[0x14] >> 4);
        mixer->voice_r  = ess_mixer_get_vol_4bit(mixer->regs[0x14]);
        mixer->master_l = ess_mixer_get_vol_4bit(mixer->regs[0x32] >> 4);
        mixer->master_r = ess_mixer_get_vol_4bit(mixer->regs[0x32]);
        mixer->fm_l     = ess_mixer_get_vol_4bit(mixer->regs[0x36] >> 4);
        mixer->fm_r     = ess_mixer_get_vol_4bit(mixer->regs[0x36]);
        mixer->cd_l     = ess_mixer_get_vol_4bit(mixer->regs[0x38] >> 4);
        mixer->cd_r     = ess_mixer_get_vol_4bit(mixer->regs[0x38]);
        mixer->line_l   = ess_mixer_get_vol_4bit(mixer->regs[0x3e] >> 4);
        mixer->line_r   = ess_mixer_get_vol_4bit(mixer->regs[0x3e]);
        mixer->auxb_l   = ess_mixer_get_vol_4bit(mixer->regs[0x3a] >> 4);
        mixer->auxb_r   = ess_mixer_get_vol_4bit(mixer->regs[0x3a]);

        mixer->output_filter  = !(mixer->regs[0xe] & 0x20);
        mixer->input_filter   = !(mixer->regs[0xc] & 0x20);
        mixer->in_filter_freq = ((mixer->regs[0xc] & 0x8) == 0) ? 3200 : 8800;
        mixer->stereo         = mixer->regs[0xe] & 2;
        if (mixer->index == 0xe)
            sb_dsp_set_stereo(&ess->dsp, val & 2);

        /* TODO: pcspeaker volume? Or is it not worth? */
    }
}

uint8_t
ess_mixer_read(uint16_t addr, void *priv)
{
    ess_t       *ess   = (ess_t *) priv;
    ess_mixer_t *mixer = &ess->mixer_sbpro;

    if (!(addr & 1))
        return mixer->index;

    switch (mixer->index) {
        case 0x00:
        case 0x04:
        case 0x0a:
        case 0x0c:
        case 0x0e:
        case 0x14:
        case 0x22:
        case 0x26:
        case 0x28:
        case 0x2e:
        case 0x02:
        case 0x06:
        case 0x30:
        case 0x32:
        case 0x36:
        case 0x38:
        case 0x3e:
            return mixer->regs[mixer->index];

        case 0x40:

            if (ess->dsp.sb_subtype != SB_SUBTYPE_ESS_ES1688) {
                uint8_t val = mixer->ess_id_str[mixer->ess_id_str_pos];
                mixer->ess_id_str_pos++;
                if (mixer->ess_id_str_pos >= 4)
                    mixer->ess_id_str_pos = 0;
                return val;
            } else {
                return mixer->regs[mixer->index];
            }

        default:
            sb_log("ess: Unknown mixer register READ: %02X\t%02X\n", mixer->index, mixer->regs[mixer->index]);
            break;
    }

    return 0x0a;
}

void
ess_mixer_reset(ess_t *ess)
{
    ess_mixer_write(4, 0, ess);
    ess_mixer_write(5, 0, ess);
}

void
ess_get_buffer_sbpro(int32_t *buffer, int len, void *priv)
{
    ess_t             *ess   = (ess_t *) priv;
    const ess_mixer_t *mixer = &ess->mixer_sbpro;
    double             out_l = 0.0;
    double             out_r = 0.0;

    sb_dsp_update(&ess->dsp);

    for (int c = 0; c < len * 2; c += 2) {
        out_l = 0.0;
        out_r = 0.0;

        /* TODO: Implement the stereo switch on the mixer instead of on the dsp? */
        if (mixer->output_filter) {
            out_l += (low_fir_sb16(0, 0, (double) ess->dsp.buffer[c]) * mixer->voice_l) / 3.0;
            out_r += (low_fir_sb16(0, 1, (double) ess->dsp.buffer[c + 1]) * mixer->voice_r) / 3.0;
        } else {
            out_l += (ess->dsp.buffer[c] * mixer->voice_l) / 3.0;
            out_r += (ess->dsp.buffer[c + 1] * mixer->voice_r) / 3.0;
        }

        /* TODO: recording CD, Mic with AGC or line in. Note: mic volume does not affect recording. */
        out_l *= mixer->master_l;
        out_r *= mixer->master_r;

        buffer[c] += (int32_t) out_l;
        buffer[c + 1] += (int32_t) out_r;
    }

    ess->dsp.pos = 0;
}

void
ess_get_music_buffer_sbpro(int32_t *buffer, int len, void *priv)
{
    ess_t             *ess     = (ess_t *) priv;
    const ess_mixer_t *mixer   = &ess->mixer_sbpro;
    double             out_l   = 0.0;
    double             out_r   = 0.0;
    const int32_t     *opl_buf = NULL;

    opl_buf = ess->opl.update(ess->opl.priv);

    for (int c = 0; c < len * 2; c += 2) {
        out_l = 0.0;
        out_r = 0.0;

        {
            out_l = (((double) opl_buf[c]) * mixer->fm_l) * 0.7171630859375;
            out_r = (((double) opl_buf[c + 1]) * mixer->fm_r) * 0.7171630859375;
            if (ess->opl_mix && ess->opl_mixer)
                ess->opl_mix(ess->opl_mixer, &out_l, &out_r);
        }

        /* TODO: recording CD, Mic with AGC or line in. Note: mic volume does not affect recording. */
        out_l *= mixer->master_l;
        out_r *= mixer->master_r;

        buffer[c] += (int32_t) out_l;
        buffer[c + 1] += (int32_t) out_r;
    }

    ess->opl.reset_buffer(ess->opl.priv);
}

void
ess_filter_cd_audio(int channel, double *buffer, void *priv)
{
    const ess_t       *ess   = (ess_t *) priv;
    const ess_mixer_t *mixer = &ess->mixer_sbpro;
    double             c;
    double             cd     = channel ? mixer->cd_r : mixer->cd_l;
    double             master = channel ? mixer->master_r : mixer->master_l;

    c       = (*buffer * cd) / 3.0;
    *buffer = c * master;
}

static void *
ess_1688_init(UNUSED(const device_t *info))
{
    /* SB Pro 2 port mappings, 220h or 240h.
       2x0 to 2x3 -> FM chip (18 voices)
       2x4 to 2x5 -> Mixer interface
       2x6, 2xA, 2xC, 2xE -> DSP chip
       2x8, 2x9, 388 and 389 FM chip (9 voices)
       2x0+10 to 2x0+13 CDROM interface. */
    ess_t   *ess  = calloc(sizeof(ess_t), 1);
    uint16_t addr = device_get_config_hex16("base");

    fm_driver_get(FM_ESFM, &ess->opl);

    sb_dsp_set_real_opl(&ess->dsp, 1);
    sb_dsp_init(&ess->dsp, SBPRO2, SB_SUBTYPE_ESS_ES1688, ess);
    sb_dsp_setaddr(&ess->dsp, addr);
    sb_dsp_setirq(&ess->dsp, device_get_config_int("irq"));
    sb_dsp_setdma8(&ess->dsp, device_get_config_int("dma"));
    sb_dsp_setdma16_8(&ess->dsp, device_get_config_int("dma"));
    sb_dsp_setdma16_supported(&ess->dsp, 0);
    ess_mixer_reset(ess);

    /* DSP I/O handler is activated in sb_dsp_setaddr */
    {
        io_sethandler(addr, 0x0004,
                      ess->opl.read, NULL, NULL,
                      ess->opl.write, NULL, NULL,
                      ess->opl.priv);
        io_sethandler(addr + 8, 0x0002,
                      ess->opl.read, NULL, NULL,
                      ess->opl.write, NULL, NULL,
                      ess->opl.priv);
        io_sethandler(0x0388, 0x0004,
                      ess->opl.read, NULL, NULL,
                      ess->opl.write, NULL, NULL,
                      ess->opl.priv);
    }

    ess->mixer_enabled = 1;
    io_sethandler(addr + 4, 0x0002,
                  ess_mixer_read, NULL, NULL,
                  ess_mixer_write, NULL, NULL,
                  ess);
    sound_add_handler(ess_get_buffer_sbpro, ess);
    music_add_handler(ess_get_music_buffer_sbpro, ess);
    sound_set_cd_audio_filter(ess_filter_cd_audio, ess);

    if (device_get_config_int("receive_input"))
        midi_in_handler(1, sb_dsp_input_msg, sb_dsp_input_sysex, &ess->dsp);
    {
        ess->mixer_sbpro.ess_id_str[0] = 0x16;
        ess->mixer_sbpro.ess_id_str[1] = 0x88;
        ess->mixer_sbpro.ess_id_str[2] = (addr >> 8) & 0xff;
        ess->mixer_sbpro.ess_id_str[3] = addr & 0xff;
    }

    ess->mpu = (mpu_t *) calloc(1, sizeof(mpu_t));
    mpu401_init(ess->mpu, 0, -1, M_UART, 1);
    sb_dsp_set_mpu(&ess->dsp, ess->mpu);

    ess->gameport      = gameport_add(&gameport_pnp_device);
    ess->gameport_addr = 0x200;
    gameport_remap(ess->gameport, ess->gameport_addr);

    return ess;
}

void
ess_close(void *priv)
{
    ess_t *ess = (ess_t *) priv;
    sb_dsp_close(&ess->dsp);

    free(ess);
}

void
ess_speed_changed(void *priv)
{
    ess_t *ess = (ess_t *) priv;

    sb_dsp_speed_changed(&ess->dsp);
}

// clang-format off
static const device_config_t ess_config[] = {
    {
        .name           =  "base",
        .description    = "Address",
        .type           = CONFIG_HEX16,
        .default_string = "",
        .default_int    = 0x220,
        .file_filter    = "",
        .spinner        = { 0 },
        .selection      = {
            {
                .description = "0x220",
                .value       = 0x220
            },
            {
                .description = "0x240",
                .value       = 0x240
            },
            { .description = "" }
        }
    },
    {
        .name           = "irq",
        .description    = "IRQ",
        .type           = CONFIG_SELECTION,
        .default_string = "",
        .default_int    = 5,
        .file_filter    = "",
        .spinner        = { 0 },
        .selection      = {
            {
                .description = "IRQ 2",
                .value       = 2
            },
            {
                .description = "IRQ 5",
                .value       = 5
            },
            {
                .description = "IRQ 7",
                .value       = 7
            },
            {
                .description = "IRQ 10",
                .value       = 10
            },
            { .description = "" }
        }
    },
    {
        .name           = "dma",
        .description    = "DMA",
        .type           = CONFIG_SELECTION,
        .default_string = "",
        .default_int    = 1,
        .file_filter    = "",
        .spinner        = { 0 },
        .selection      = {
            {
                .description = "DMA 0",
                .value       = 0
            },
            {
                .description = "DMA 1",
                .value       = 1
            },
            {
                .description = "DMA 3",
                .value       = 3
            },
            { .description = "" }
        }
    },
    {
        .name           = "opl",
        .description    = "Enable OPL",
        .type           = CONFIG_BINARY,
        .default_string = "",
        .default_int    = 1
    },
    {
        .name           = "receive_input",
        .description    = "Receive input (SB MIDI)",
        .type           = CONFIG_BINARY,
        .default_string = "",
        .default_int    = 1
    },
    { .name = "", .description = "", .type = CONFIG_END }
};
// clang-format on

const device_t ess_1688_device = {
    .name          = "ESS Technology ES1688",
    .internal_name = "ess_es1688",
    .flags         = DEVICE_ISA,
    .local         = 0,
    .init          = ess_1688_init,
    .close         = ess_close,
    .reset         = NULL,
    { .available = NULL },
    .speed_changed = ess_speed_changed,
    .force_redraw  = NULL,
    .config        = ess_config
};
