/*
 * Copyright (C) 2016 The Android Open Source Project
 * Copyright (C) 2018 Shane Francis
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "lights.sony"

#include <log/log.h>

#include "Light.h"

namespace android {
    namespace hardware {
        namespace light {
            namespace V2_0 {
                namespace implementation {

                    static_assert(LIGHT_FLASH_NONE == static_cast<int>(Flash::NONE),
                                  "Flash::NONE must match legacy value.");
                    static_assert(LIGHT_FLASH_TIMED == static_cast<int>(Flash::TIMED),
                                  "Flash::TIMED must match legacy value.");
                    static_assert(LIGHT_FLASH_HARDWARE == static_cast<int>(Flash::HARDWARE),
                                  "Flash::HARDWARE must match legacy value.");

                    static_assert(BRIGHTNESS_MODE_USER == static_cast<int>(Brightness::USER),
                                  "Brightness::USER must match legacy value.");
                    static_assert(BRIGHTNESS_MODE_SENSOR == static_cast<int>(Brightness::SENSOR),
                                  "Brightness::SENSOR must match legacy value.");
                    static_assert(BRIGHTNESS_MODE_LOW_PERSISTENCE ==
                                  static_cast<int>(Brightness::LOW_PERSISTENCE),
                                  "Brightness::LOW_PERSISTENCE must match legacy value.");

                    Light *Light::sInstance = nullptr;

                    Light::Light() {
                        ALOGI("%s",__func__);
                        openHal();
                        sInstance = this;
                    }

                    void Light::openHal(){
                        ALOGI("%s : Setup HAL",__func__);
                        mDevice = (lights_t *) malloc(sizeof(lights_t));
                        memset(mDevice, 0, sizeof(lights_t));

                        mDevice->g_last_backlight_mode = BRIGHTNESS_MODE_USER;
                        mDevice->g_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
                        mDevice->g_lcd_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;

                        mDevice->backlight_bits = (readInt(LCD_MAX_FILE) == 4095 ? 12 : 8);
                    }

                    // Methods from ::android::hardware::light::V2_0::ILight follow.
                    Return<Status> Light::setLight(Type type, const LightState &state) {

                        switch (type) {
                            case Type::BACKLIGHT:
                                ALOGD("%s : Type::BACKLIGHT",__func__);
                                setLightBacklight(state);
                                break;
                            case Type::BATTERY:
                                ALOGD("%s : Type::BATTERY",__func__);
                                setLightBattery(state);
                                break;
                            case Type::NOTIFICATIONS:
                                ALOGD("%s : Type::NOTIFICATIONS",__func__);
                                setLightNotifications(state);
                                break;
                            default:
                                ALOGE("%s : Unknown light type",__func__);
                                return Status::LIGHT_NOT_SUPPORTED;
                        }
                        return Status::SUCCESS;
                    }

                    ILight *Light::getInstance() {
                        if (!sInstance) {
                            sInstance = new Light();
                        }
                        return sInstance;
                    }

                    int Light::writeInt(char const *path, int value) {
                        int fd;
                        static int already_warned = 0;

                        fd = open(path, O_WRONLY);
                        if (fd >= 0) {
                            char buffer[20] = {0,};
                            int bytes = snprintf(buffer, sizeof(buffer), "%d\n", value);
                            ssize_t amt = write(fd, buffer, (size_t) bytes);
                            close(fd);
                            return amt == -1 ? -errno : 0;
                        } else {
                            if (already_warned == 0) {
                                ALOGE("write_int failed to open %s\n", path);
                                already_warned = 1;
                            }
                            return -errno;
                        }
                    }

                    int Light::readInt(char const *path) {
                        static int already_warned = 0;
                        int fd;

                        fd = open(path, O_RDONLY);
                        if (fd >= 0) {
                            char read_str[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
                            ssize_t err = read(fd, &read_str, sizeof(read_str));
                            close(fd);
                            return err < 2 ? -errno : atoi(read_str);
                        } else {
                            if (already_warned == 0) {
                                ALOGE("read_int failed to open %s\n", path);
                                already_warned = 1;
                            }
                            return -errno;
                        };
                    }

                    int Light::isLit(const LightState &state) {
                        return state.color & 0x00ffffff;
                    }

                    int Light::rgbToBrightness(const LightState &state) {
                        int color = state.color & 0x00ffffff;
                        return ((77 * ((color >> 16) & 0x00ff))
                                + (150 * ((color >> 8) & 0x00ff)) + (29 * (color & 0x00ff))) >> 8;
                    }

                    int Light::setLightBacklight(const LightState &state) {
                        int err = 0;
                        int brightness = rgbToBrightness(state);
                        unsigned int lpEnabled = state.brightnessMode == Brightness::LOW_PERSISTENCE;

                        if (!mDevice) {
                            return -1;
                        }

                        pthread_mutex_lock(&mDevice->g_lcd_lock);

#ifdef LOW_PERSISTENCE_DISPLAY
                        int currState = static_cast<int>(state.brightnessMode);
                        // If we're not in lp mode and it has been enabled or if we are in lp mode
                            // and it has been disabled send an ioctl to the display with the update
                            if ((mDevice->g_last_backlight_mode != currState && lpEnabled) ||
                                    (!lpEnabled && mDevice->g_last_backlight_mode == BRIGHTNESS_MODE_LOW_PERSISTENCE)) {
                                if ((err = writeInt(PERSISTENCE_FILE, lpEnabled)) != 0) {
                                    ALOGE("%s: Failed to write to %s: %s\n", __FUNCTION__, PERSISTENCE_FILE,
                                            strerror(errno));
                                }
                                if (lpEnabled != 0) {
                                    // Try to get the brigntess though property, otherwise it will
                                    // set the default brightness, which is defined in BoardConfig.mk.
                                    brightness = property_get_int32(LP_MODE_BRIGHTNESS_PROPERTY,
                                            DEFAULT_LOW_PERSISTENCE_MODE_BRIGHTNESS);
                                }
                            }
                            mDevice->g_last_backlight_mode = static_cast<int>(state.brightnessMode);
#endif

                        if (!err) {
                            if (mDevice->backlight_bits > 8)
                                brightness = brightness << (mDevice->backlight_bits - 8);

                            err = writeInt(LCD_FILE, brightness);
                        }

                        pthread_mutex_unlock(&mDevice->g_lcd_lock);
                        return err;
                    }


                    int Light::setSpeakerLightLocked(const LightState& state) {
                        int red, green, blue;
                        int blink;
                        int onMS, offMS;
                        unsigned int colorRGB;

                        if(!mDevice) {
                            return -1;
                        }

                        switch (state.flashMode) {
                            case Flash::TIMED:
                                onMS = state.flashOnMs;
                                offMS = state.flashOffMs;
                                break;
                            case Flash::NONE:
                            default:
                                onMS = 0;
                                offMS = 0;
                                break;
                        }

                        colorRGB = state.color;

#if 0
                        ALOGD("set_speaker_light_locked mode %d, colorRGB=%08X, onMS=%d, offMS=%d\n",
			state->flashMode, colorRGB, onMS, offMS);
#endif

                        red = (colorRGB >> 16) & 0xFF;
                        green = (colorRGB >> 8) & 0xFF;
                        blue = colorRGB & 0xFF;

                        if (onMS > 0 && offMS > 0) {
                            /*
                             * if ON time == OFF time
                             *   use blink mode 2
                             * else
                             *   use blink mode 1
                             */
                            if (onMS == offMS)
                                blink = 2;
                            else
                                blink = 1;
                        } else {
                            blink = 0;
                        }

                        if (blink) {
                            if (red) {
                                if (writeInt(RED_BLINK_FILE, blink))
                                    writeInt(RED_LED_FILE, 0);
                            }
                            if (green) {
                                if (writeInt(GREEN_BLINK_FILE, blink))
                                    writeInt(GREEN_LED_FILE, 0);
                            }
                            if (blue) {
                                if (writeInt(BLUE_BLINK_FILE, blink))
                                    writeInt(BLUE_LED_FILE, 0);
                            }
                        } else {
                            writeInt(RED_LED_FILE, red);
                            writeInt(GREEN_LED_FILE, green);
                            writeInt(BLUE_LED_FILE, blue);
                        }

                        return 0;
                    }

                    void Light::handleSpeakerBatteryLocked() {
                        if (isLit(batteryState)) {
                            setSpeakerLightLocked(batteryState);
                        } else {
                            setSpeakerLightLocked(notificationState);
                        }
                    }

                    int Light::setLightBattery(const LightState& state) {
                        if(!mDevice) {
                            return -1;
                        }

                        pthread_mutex_lock(&mDevice->g_lock);
                        batteryState = state;
                        handleSpeakerBatteryLocked();
                        pthread_mutex_unlock(&mDevice->g_lock);
                        return 0;
                    }

                    int Light::setLightNotifications(const LightState& state) {
                        if(!mDevice) {
                            return -1;
                        }

                        pthread_mutex_lock(&mDevice->g_lock);
                        notificationState = state;
                        handleSpeakerBatteryLocked();
                        pthread_mutex_unlock(&mDevice->g_lock);
                        return 0;
                    }

                    const static std::map<Type, const char *> kLogicalLights = {
                            {Type::BACKLIGHT,     LIGHT_ID_BACKLIGHT},
                            {Type::BATTERY,       LIGHT_ID_BATTERY},
                            {Type::NOTIFICATIONS, LIGHT_ID_NOTIFICATIONS}
                    };

                    Return<void> Light::getSupportedTypes(getSupportedTypes_cb _hidl_cb) {
                        Type *types = new Type[kLogicalLights.size()];

                        int idx = 0;
                        for (auto const &pair : kLogicalLights) {
                            Type type = pair.first;

                            types[idx++] = type;
                        }

                        {
                            hidl_vec<Type> hidl_types{};
                            hidl_types.setToExternal(types, kLogicalLights.size());

                            _hidl_cb(hidl_types);
                        }

                        delete[] types;

                        return Void();
                    }
                } // namespace implementation
            }  // namespace V2_0
        }  // namespace light
    }  // namespace hardware
}  // namespace android