/*
 * QEMU SMBus Generic Storage Device
 *
 * Copyright (c) 2020 Mike Davis
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/qdev-properties.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/smbus_slave.h"
#include "smbus.h"
#include "qapi/error.h"
#include "hw/loader.h"
#include "migration/vmstate.h"
#include "system/reset.h"
#include "eeprom_generation.h"
#include "ui/xemu-widescreen.h"

#define TYPE_SMBUS_STORAGE "smbus-storage"
#define SMBUS_STORAGE(obj) OBJECT_CHECK(SMBusStorageDevice, (obj), \
    TYPE_SMBUS_STORAGE)

//#define DEBUG
#ifdef DEBUG
# define DPRINTF(format, ...) printf(format, ## __VA_ARGS__)
#else
# define DPRINTF(format, ...) do { } while (0)
#endif

typedef struct SMBusStorageDevice {
    SMBusDevice smbusdev;
    char* file;
    uint8_t *data;
    uint32_t size;
    uint8_t addr;
    uint32_t offset;
    bool persist;
    /* The guest's own EEPROM video flags, kept in the file. */
    uint32_t file_video_flags;
    /* The in-memory video flags are forced to 16:9. */
    bool video_forced;
} SMBusStorageDevice;

static bool smbus_storage_is_eeprom(SMBusStorageDevice *s)
{
    return s->addr == 0x54 && s->size == sizeof(XboxEEPROM);
}

/*
 * Takes the guest's video flags from the in-memory image. Aspect bits
 * that read 16:9 while forced are not the guest's choice and keep the
 * file's.
 */
static void smbus_storage_update_video_flags(SMBusStorageDevice *s,
                                             bool forced)
{
    const uint32_t aspect = XBOX_EEPROM_VIDEO_WIDESCREEN |
                            XBOX_EEPROM_VIDEO_LETTERBOX;
    uint32_t flags = ldl_le_p(s->data + XBOX_EEPROM_VIDEO_FLAGS_OFFSET);

    if (forced && (flags & aspect) == XBOX_EEPROM_VIDEO_WIDESCREEN) {
        s->file_video_flags = (flags & ~aspect) |
                              (s->file_video_flags & aspect);
    } else {
        s->file_video_flags = flags;
    }
    s->video_forced = s->file_video_flags != flags;
}

/*
 * The kernel reads the television settings from the EEPROM when it boots,
 * so the Auto and 16:9 aspect ratios report a 16:9 set before every boot.
 * Only the in-memory copy changes; the file keeps the guest's own setting,
 * which is restored at the next boot under 4:3 or Native.
 */
static void smbus_storage_apply_video_settings(SMBusStorageDevice *s)
{
    if (!smbus_storage_is_eeprom(s)) {
        return;
    }
    if (xemu_get_widescreen_tv()) {
        if (xbox_eeprom_set_video_flags(s->data,
                                        XBOX_EEPROM_VIDEO_WIDESCREEN,
                                        XBOX_EEPROM_VIDEO_LETTERBOX)) {
            s->video_forced = true;
        }
    } else if (s->video_forced) {
        xbox_eeprom_set_video_flags(s->data, s->file_video_flags,
                                    ~s->file_video_flags);
        s->video_forced = false;
    }
}

static void smbus_storage_reset(void *opaque)
{
    smbus_storage_apply_video_settings(opaque);
}

static void smbus_storage_realize(DeviceState *dev, Error **errp)
{
    SMBusStorageDevice *s = SMBUS_STORAGE(dev);
    qdev_prop_set_uint8(dev, "address", s->addr);
    s->data = g_malloc0(s->size);
    s->offset = 0;

    if (s->file) {
        int size = get_image_size(s->file, NULL);
        if (size != s->size) {
            error_setg(errp, "%s: file '%s' size of %d, expected %d\n",
                __func__, s->file, size, s->size);
            return;
        }

        int fd = qemu_open(s->file, O_RDONLY | O_BINARY, NULL);
        if (fd < 0) {
            error_setg(errp, "%s: file '%s' could not be opened\n",
                __func__, s->file);
            return;
        }

        int rc = read(fd, s->data, s->size);
        if (rc != s->size) {
            error_setg(errp, "%s: file '%s' read failure\n", __func__, s->file);
            close(fd);
            return;
        }
        close(fd);
        if (smbus_storage_is_eeprom(s)) {
            s->file_video_flags =
                ldl_le_p(s->data + XBOX_EEPROM_VIDEO_FLAGS_OFFSET);
        }
        smbus_storage_apply_video_settings(s);
        qemu_register_reset(smbus_storage_reset, s);
    } else {
        error_setg(errp, "%s: file unspecified\n", __func__);
    }
}

static int smbus_storage_write_data(SMBusDevice *dev, uint8_t *buf, uint8_t len)
{
    SMBusStorageDevice *s = SMBUS_STORAGE(dev);

    DPRINTF("%s: addr=0x%02x cmd=0x%02x val=0x%02x\n",
        __func__, s->addr, buf[0], buf[1]);

    /* len is guaranteed to be > 0 */
    s->offset = buf[0];
    buf++;
    len--;

    bool changed = false;
    bool video_written = false;
    for (; len > 0; len--) {
        changed = true;
        if (s->offset >= XBOX_EEPROM_VIDEO_FLAGS_OFFSET &&
            s->offset < XBOX_EEPROM_VIDEO_FLAGS_OFFSET + 4) {
            video_written = true;
        }
        s->data[s->offset] = *buf++;
        DPRINTF("%s: addr=0x%02x off=0x%02x, data=0x%02x\n",
            __func__, s->addr, s->offset, s->data[s->offset]);
        s->offset = (s->offset + 1) % s->size;
    }

    if (video_written && smbus_storage_is_eeprom(s)) {
        smbus_storage_update_video_flags(s, s->video_forced);
    }

    if (changed && s->file && s->persist) {
        uint8_t restored[sizeof(XboxEEPROM)];
        const uint8_t *image = s->data;

        /* The file keeps the guest's video setting, not the forced one. */
        if (s->video_forced) {
            memcpy(restored, s->data, sizeof(restored));
            xbox_eeprom_set_video_flags(restored, s->file_video_flags,
                                        ~s->file_video_flags);
            image = restored;
        }

        int fd = qemu_open(s->file, O_WRONLY | O_BINARY, NULL);
        if (fd < 0) {
            DPRINTF("%s: file '%s' could not be opened\n", __func__, s->file);
            return -1;
        }

        int wc = write(fd, image, s->size);
        if (wc != s->size) {
            DPRINTF( "%s: file '%s' write failure\n", __func__, s->file);
            close(fd);
            return -1;
        }
        close(fd);
    }

    return 0;
}

static uint8_t smbus_storage_receive_byte(SMBusDevice *dev)
{
    SMBusStorageDevice *s = SMBUS_STORAGE(dev);

    uint8_t val = s->data[s->offset];
    DPRINTF("%s: addr=0x%02x off=0x%02x val=0x%02x\n",
        __func__, s->addr, s->offset, val);
    s->offset = (s->offset + 1) % s->size;

    return val;
}

/* A snapshot's image may hold the forced video flags. */
static int smbus_storage_post_load(void *opaque, int version_id)
{
    SMBusStorageDevice *s = opaque;

    if (smbus_storage_is_eeprom(s)) {
        smbus_storage_update_video_flags(s, true);
    }
    return 0;
}

static const VMStateDescription vmstate_smbus_storage = {
    .name = "smbus-storage",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = smbus_storage_post_load,
    .fields      = (VMStateField[]) {
        VMSTATE_SMBUS_DEVICE(smbusdev, SMBusStorageDevice),
        VMSTATE_VBUFFER_UINT32(data, SMBusStorageDevice, 1, NULL, size),
        VMSTATE_UINT32(offset, SMBusStorageDevice),
        VMSTATE_END_OF_LIST()
    }
};

// default xbox eeprom with persistence
static const Property smbus_storage_props[] = {
    DEFINE_PROP_UINT8("addr", SMBusStorageDevice, addr, 0x54),
    DEFINE_PROP_UINT32("size", SMBusStorageDevice, size, 256),
    DEFINE_PROP_BOOL("persist", SMBusStorageDevice, persist, true),
    DEFINE_PROP_STRING("file", SMBusStorageDevice, file),
};

static void smbus_storage_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SMBusDeviceClass *sc = SMBUS_DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_smbus_storage;
    dc->realize = smbus_storage_realize;
    sc->receive_byte = smbus_storage_receive_byte;
    sc->write_data = smbus_storage_write_data;
    device_class_set_props(dc, smbus_storage_props);
}

static TypeInfo smbus_storage_info = {
    .name = TYPE_SMBUS_STORAGE,
    .parent = TYPE_SMBUS_DEVICE,
    .instance_size = sizeof(SMBusStorageDevice),
    .class_init = smbus_storage_class_init,
};

static void smbus_storage_register_devices(void)
{
    type_register_static(&smbus_storage_info);
}

type_init(smbus_storage_register_devices)
