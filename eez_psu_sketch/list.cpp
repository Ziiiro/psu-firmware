/*
 * EEZ PSU Firmware
 * Copyright (C) 2017-present, Envox d.o.o.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
 
#include "psu.h"
#include "list.h"
#include "trigger.h"
#include "channel_dispatcher.h"
#if OPTION_SD_CARD
#include "sd_card.h"
#endif

namespace eez {
namespace psu {
namespace list {

static struct {
    float dwellList[MAX_LIST_LENGTH];
    uint16_t dwellListLength;

    float voltageList[MAX_LIST_LENGTH];
    uint16_t voltageListLength;

    float currentList[MAX_LIST_LENGTH];
    uint16_t currentListLength;

    uint16_t count;

    bool changed;
} g_channelsLists[CH_MAX];

static struct {
    int32_t counter;
    int16_t it;
    uint32_t nextPointTime;
} g_execution[CH_NUM];

static bool g_active;

////////////////////////////////////////////////////////////////////////////////

void init() {
    reset();
}

void resetChannelList(Channel &channel) {
    int i = channel.index - 1;

    g_channelsLists[i].voltageListLength = 0;
    g_channelsLists[i].currentListLength = 0;
    g_channelsLists[i].dwellListLength = 0;

    g_channelsLists[i].changed = false;

    g_channelsLists[i].count = 1;

    g_execution[i].counter = -1;
}

void reset() {
    for (int i = 0; i < CH_NUM; ++i) {
        resetChannelList(Channel::get(i));
    }
}

void setDwellList(Channel &channel, float *list, uint16_t listLength) {
    memcpy(g_channelsLists[channel.index - 1].dwellList, list, listLength * sizeof(float));
    g_channelsLists[channel.index - 1].dwellListLength = listLength;
    g_channelsLists[channel.index - 1].changed = true;
}

float *getDwellList(Channel &channel, uint16_t *listLength) {
    *listLength = g_channelsLists[channel.index - 1].dwellListLength;
    return g_channelsLists[channel.index - 1].dwellList;
}

void setVoltageList(Channel &channel, float *list, uint16_t listLength) {
    memcpy(g_channelsLists[channel.index - 1].voltageList, list, listLength * sizeof(float));
    g_channelsLists[channel.index - 1].voltageListLength = listLength;
    g_channelsLists[channel.index - 1].changed = true;
}

float *getVoltageList(Channel &channel, uint16_t *listLength) {
    *listLength = g_channelsLists[channel.index - 1].voltageListLength;
    return g_channelsLists[channel.index - 1].voltageList;
}

void setCurrentList(Channel &channel, float *list, uint16_t listLength) {
    memcpy(g_channelsLists[channel.index - 1].currentList, list, listLength * sizeof(float));
    g_channelsLists[channel.index - 1].currentListLength = listLength;
    g_channelsLists[channel.index - 1].changed = true;
}

float *getCurrentList(Channel &channel, uint16_t *listLength) {
    *listLength = g_channelsLists[channel.index - 1].currentListLength;
    return g_channelsLists[channel.index - 1].currentList;
}

bool getListsChanged(Channel &channel) {
    return g_channelsLists[channel.index - 1].changed;
}

void setListsChanged(Channel &channel, bool changed) {
    g_channelsLists[channel.index - 1].changed = changed;
}

uint16_t getListCount(Channel &channel) {
    return g_channelsLists[channel.index - 1].count;
}

void setListCount(Channel &channel, uint16_t value) {
    g_channelsLists[channel.index - 1].count = value;
}

bool areListLengthsEquivalent(uint16_t size1, uint16_t size2) {
    return size1 != 0 && size2 != 0 && (size1 == 1 || size2 == 1 || size1 == size2);
}

bool areListLengthsEquivalent(uint16_t size1, uint16_t size2, uint16_t size3) {
    return list::areListLengthsEquivalent(size1, size2) &&
           list::areListLengthsEquivalent(size1, size3) &&
           list::areListLengthsEquivalent(size2, size3);
}

bool areListLengthsEquivalent(Channel &channel) {
    return list::areListLengthsEquivalent(
        g_channelsLists[channel.index - 1].dwellListLength,
        g_channelsLists[channel.index - 1].voltageListLength,
        g_channelsLists[channel.index - 1].currentListLength
    );
}

bool areVoltageAndDwellListLengthsEquivalent(Channel &channel) {
    return areListLengthsEquivalent(g_channelsLists[channel.index - 1].voltageListLength, g_channelsLists[channel.index - 1].dwellListLength);
}

bool areCurrentAndDwellListLengthsEquivalent(Channel &channel) {
    return areListLengthsEquivalent(g_channelsLists[channel.index - 1].currentListLength, g_channelsLists[channel.index - 1].dwellListLength);
}

bool areVoltageAndCurrentListLengthsEquivalent(Channel &channel) {
    return areListLengthsEquivalent(g_channelsLists[channel.index - 1].voltageListLength, g_channelsLists[channel.index - 1].currentListLength);
}

bool loadList(Channel &channel, const char *filePath, int *err) {
#if OPTION_SD_CARD
    File file = SD.open(filePath, FILE_READ);

    if (!file) {
        // TODO more specific error
        if (err) {
            *err = SCPI_ERROR_EXECUTION_ERROR;
        }
        return false;
    }

    float dwellList[MAX_LIST_LENGTH];
    uint16_t dwellListLength = 0;

    float voltageList[MAX_LIST_LENGTH];
    uint16_t voltageListLength = 0;

    float currentList[MAX_LIST_LENGTH];
    uint16_t currentListLength = 0;

    bool success = true;

    for (int i = 0; i < MAX_LIST_LENGTH; ++i) {
        sd_card::matchZeroOrMoreSpaces(file);
        if (!file.available()) {
            break;
        }

        float value;

        if (sd_card::match(file, LIST_CSV_FILE_NO_VALUE_CHAR)) {
            if (i < dwellListLength) {
                success = false;
                break;
            }
        } else if (sd_card::match(file, value)) {
            if (i == dwellListLength) {
                dwellList[i] = value;
                dwellListLength = i + 1;
            } else {
                success = false;
                break;
            }
        } else {
            success = false;
            break;
        }

        sd_card::match(file, CSV_SEPARATOR);

        if (sd_card::match(file, LIST_CSV_FILE_NO_VALUE_CHAR)) {
            if (i < voltageListLength) {
                success = false;
                break;
            }
        } else if (sd_card::match(file, value)) {
            if (i == voltageListLength) {
                voltageList[i] = value;
                ++voltageListLength;
            } else {
                success = false;
                break;
            }
        } else {
            success = false;
            break;
        }

        sd_card::match(file, CSV_SEPARATOR);

        if (sd_card::match(file, LIST_CSV_FILE_NO_VALUE_CHAR)) {
            if (i < currentListLength) {
                success = false;
                break;
            }
        } else if (sd_card::match(file, value)) {
            if (i == currentListLength) {
                currentList[i] = value;
                ++currentListLength;
            } else {
                success = false;
                break;
            }
        } else {
            success = false;
            break;
        }
    }

    file.close();

    if (success) {
        setDwellList(channel, dwellList, dwellListLength);
        setVoltageList(channel, voltageList, voltageListLength);
        setCurrentList(channel, currentList, currentListLength);
    } else {
        // TODO more specific error
        if (err) {
            *err = SCPI_ERROR_EXECUTION_ERROR;
        }
    }

    return success;
#else
    if (err) {
        *err = SCPI_ERROR_OPTION_NOT_INSTALLED;
    }
    return false;
#endif
}

bool saveList(Channel &channel, const char *filePath, int *err) {
#if OPTION_SD_CARD
    sd_card::makeParentDir(filePath);

    SD.remove(filePath);

    File file = SD.open(filePath, FILE_WRITE);

    if (!file) {
        // TODO more specific error
        if (err) {
            *err = SCPI_ERROR_EXECUTION_ERROR;
        }
        return false;
    }

    for (
        int i = 0;
        i < g_channelsLists[channel.index - 1].dwellListLength ||
        i < g_channelsLists[channel.index - 1].voltageListLength ||
        i < g_channelsLists[channel.index - 1].currentListLength; 
        ++i) 
    {
        if (i < g_channelsLists[channel.index - 1].dwellListLength) {
            file.print(g_channelsLists[channel.index - 1].dwellList[i], 6);
        } else {
            file.print(LIST_CSV_FILE_NO_VALUE_CHAR);
        }

        file.print(CSV_SEPARATOR);

        if (i < g_channelsLists[channel.index - 1].voltageListLength) {
            file.print(g_channelsLists[channel.index - 1].voltageList[i], 6);
        } else {
            file.print(LIST_CSV_FILE_NO_VALUE_CHAR);
        }
        
        file.print(CSV_SEPARATOR);
        
        if (i < g_channelsLists[channel.index - 1].currentListLength) {
            file.print(g_channelsLists[channel.index - 1].currentList[i], 6);
        } else {
            file.print(LIST_CSV_FILE_NO_VALUE_CHAR);
        }

        file.print('\n');
    }

    file.close();

    return true;
#else
    if (err) {
        *err = SCPI_ERROR_OPTION_NOT_INSTALLED;
    }
    return false;
#endif
}

void executionStart(Channel &channel) {
    g_execution[channel.index - 1].it = -1;
    g_execution[channel.index - 1].counter = g_channelsLists[channel.index - 1].count;
}

int maxListsSize(Channel &channel) {
    uint16_t maxSize = 0;

    if (g_channelsLists[channel.index - 1].voltageListLength > maxSize) {
        maxSize = g_channelsLists[channel.index - 1].voltageListLength;
    }

    if (g_channelsLists[channel.index - 1].currentListLength > maxSize) {
        maxSize = g_channelsLists[channel.index - 1].currentListLength;
    }

    if (g_channelsLists[channel.index - 1].dwellListLength > maxSize) {
        maxSize = g_channelsLists[channel.index - 1].dwellListLength;
    }

    return maxSize;
}

void tick(uint32_t tick_usec) {
#if CONF_DEBUG_VARIABLES
    debug::g_listTickDuration.tick(tick_usec);
#endif

    g_active = false;

    for (int i = 0; i < CH_NUM; ++i) {
        Channel &channel = Channel::get(i);
        if (g_execution[i].counter >= 0) {
            g_active = true;

            bool set = false;

            if (g_execution[i].it == -1) {
                set = true;
            } else {
                int32_t diff = g_execution[i].nextPointTime - tick_usec;
                if (diff <= 0) {
                    set = true;
                }
            }

            if (set) {
                if (++g_execution[i].it == maxListsSize(channel)) {
                    if (g_execution[i].counter > 0) {
                        if (--g_execution[i].counter == 0) {
                            g_execution[i].counter = -1;
                            trigger::setTriggerFinished(channel);
                            return;
                        }
                    }

                    g_execution[i].it = 0;
                }

                float voltage = g_channelsLists[i].voltageList[g_execution[i].it % g_channelsLists[i].voltageListLength];

	            if (voltage > channel_dispatcher::getULimit(channel)) {
                    generateError(SCPI_ERROR_VOLTAGE_LIMIT_EXCEEDED);
                    abort();
                    return;
	            }

	            if (voltage * channel_dispatcher::getISet(channel) > channel_dispatcher::getPowerLimit(channel)) {
                    generateError(SCPI_ERROR_POWER_LIMIT_EXCEEDED);
                    abort();
                    return;
                }

                channel_dispatcher::setVoltage(channel, voltage);

                float current = g_channelsLists[i].currentList[g_execution[i].it % g_channelsLists[i].currentListLength];

                if (current > channel_dispatcher::getILimit(channel)) {
                    generateError(SCPI_ERROR_CURRENT_LIMIT_EXCEEDED);
                    abort();
                    return;
	            }

                if (current * channel_dispatcher::getUSet(channel) > channel_dispatcher::getPowerLimit(channel)) {
                    generateError(SCPI_ERROR_POWER_LIMIT_EXCEEDED);
                    abort();
                    return;
                }

                channel_dispatcher::setCurrent(channel, current);

                uint32_t dwell = (uint32_t)round(g_channelsLists[i].dwellList[g_execution[i].it % g_channelsLists[i].dwellListLength] * 1000000L);
                g_execution[i].nextPointTime = tick_usec + dwell;
            }
        }
    }
}

bool isActive() {
    return g_active;
}

void abort() {
    for (int i = 0; i < CH_NUM; ++i) {
        g_execution[i].counter = -1;
    }
}

}
}
} // namespace eez::psu::list