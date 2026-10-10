#include "nt7508.h"

#include <algorithm>

NT7508::NT7508()
{
    OnInputPin += [this](PinEvent event)
    {
        if (event.pin == NT7508_PIN_DC)
            is_data_mode = event.value;
    };
}

void NT7508::Receive(uint8_t data)
{
    std::scoped_lock lock(draw_info_mutex);

    if (is_data_mode)
    {
        if (column >= NT7508_TOTAL_COLUMNS)
            return;

        const uint16_t address = (page * NT7508_TOTAL_COLUMNS * NT7508_COLUMN_SIZE) +
                                 (column * NT7508_COLUMN_SIZE) + offset;
        draw_info.vram.Write8(address, data);

        if (offset == NT7508_COLUMN_SIZE - 1)
            column++;

        offset = (offset + 1) % NT7508_COLUMN_SIZE;
        return;
    }

    switch (state)
    {
    case NT7508State::IDLE:
        HandleCommand(data);
        break;
    case NT7508State::SET_CONTRAST:
        draw_info.contrast = data & 0x3F;
        state = NT7508State::IDLE;
        break;
    case NT7508State::SET_INITIAL_DISPLAY_LINE:
        draw_info.initial_display_line = data & NT7508_INITIAL_DISPLAY_LINE_MAX;
        state = NT7508State::IDLE;
        break;
    case NT7508State::IGNORE_VALUE:
        state = NT7508State::IDLE;
        break;
    }
}

uint8_t NT7508::Transmit()
{
    return 0xFF;
}

NT7508DrawInfo NT7508::GetDrawInfoSnapshot() const
{
    std::scoped_lock lock(draw_info_mutex);
    return draw_info;
}

void NT7508::HandleCommand(uint8_t data)
{
    if (data >= NT7508_CMD_COL_LOW_MIN && data <= NT7508_CMD_COL_LOW_MAX)
    {
        column = (column & 0xF0) | (data & 0x0F);
        offset = 0;
    }
    else if (data >= NT7508_CMD_COL_HIGH_MIN && data <= NT7508_CMD_COL_HIGH_MAX)
    {
        column = (column & 0x0F) | ((data & 0x07) << 4);
        offset = 0;
    }
    else if (data >= NT7508_CMD_SET_INITIAL_DISPLAY_LINE_MIN && data <= NT7508_CMD_SET_INITIAL_DISPLAY_LINE_MAX)
    {
        state = NT7508State::SET_INITIAL_DISPLAY_LINE;
    }
    else if (data >= NT7508_CMD_SET_INITIAL_COM0_MIN && data <= NT7508_CMD_SET_N_LINE_INVERSION_MAX)
    {
        state = NT7508State::IGNORE_VALUE;
    }
    else if (data == NT7508_CMD_SET_CONTRAST)
    {
        state = NT7508State::SET_CONTRAST;
    }
    else if (data >= NT7508_CMD_SET_GRAY_PULSE_WIDTH_MIN && data <= NT7508_CMD_SET_GRAY_PULSE_WIDTH_MAX)
    {
        state = NT7508State::IGNORE_VALUE;
    }
    else if (data == NT7508_CMD_POWER_SAVE_ON)
    {
        draw_info.power_save = true;
    }
    else if (data >= NT7508_CMD_SET_PAGE_MIN && data <= NT7508_CMD_SET_PAGE_MAX)
    {
        page = data & 0x0F;
    }
    else if (data == NT7508_CMD_POWER_SAVE_OFF)
    {
        draw_info.power_save = false;
    }
    else if (data == NT7508_CMD_SET_TEMPERATURE_COEFFICIENT ||
             data == NT7508_CMD_SET_FRAME_FREQUENCY ||
             data == NT7508_CMD_SET_OSCILLATOR_SOURCE)
    {
        state = NT7508State::IGNORE_VALUE;
    }
}

void NT7508::SaveEmulatorState(std::ostream& stream) const
{
    const uint8_t state_value = static_cast<uint8_t>(state);
    stream.write(reinterpret_cast<const char*>(&state_value), sizeof(state_value));
    stream.write(reinterpret_cast<const char*>(&column), sizeof(column));
    stream.write(reinterpret_cast<const char*>(&offset), sizeof(offset));
    stream.write(reinterpret_cast<const char*>(&page), sizeof(page));
    stream.write(reinterpret_cast<const char*>(&is_data_mode), sizeof(is_data_mode));
}

bool NT7508::LoadEmulatorState(std::istream& stream)
{
    uint8_t state_value = 0;
    stream.read(reinterpret_cast<char*>(&state_value), sizeof(state_value));
    stream.read(reinterpret_cast<char*>(&column), sizeof(column));
    stream.read(reinterpret_cast<char*>(&offset), sizeof(offset));
    stream.read(reinterpret_cast<char*>(&page), sizeof(page));
    stream.read(reinterpret_cast<char*>(&is_data_mode), sizeof(is_data_mode));
    if (!stream)
        return false;

    if (state_value > static_cast<uint8_t>(NT7508State::IGNORE_VALUE))
        state_value = static_cast<uint8_t>(NT7508State::IDLE);

    state = static_cast<NT7508State>(state_value);
    column = static_cast<uint8_t>(std::min<uint16_t>(column, NT7508_TOTAL_COLUMNS));
    offset %= NT7508_COLUMN_SIZE;
    page %= NT7508_TOTAL_PAGES;
    return true;
}
