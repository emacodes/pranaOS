/*
 * Copyright (c) 2021, Krisna Pranav
 *
 * SPDX-License-Identifier: BSD-2-Clause
*/

#pragma once

// includes
#include <base/NonnullRefPtr.h>
#include <base/Types.h>
#include <kernel/Time/HardwareTimer.h>

namespace Kernel {

#define TIMER0_CTL 0x40
#define TIMER1_CTL 0x41
#define TIMER2_CTL 0x42
#define PIT_CTL 0x43

#define TIMER0_SELECT 0x00
#define TIMER1_SELECT 0x40
#define TIMER2_SELECT 0x80

#define MODE_COUNTDOWN 0x00
#define MODE_ONESHOT 0x02
#define MODE_RATE 0x04
#define MODE_SQUARE_WAVE 0x06

#define WRITE_WORD 0x30

#define BASE_FREQUENCY 1193182

class PIT final : public HardwareTimer<IRQHandler> {
public:
    static NonnullRefPtr<PIT> initialize(Function<void(const RegisterState&)>);
    virtual HardwareTimerType timer_type() const override { return HardwareTimerType::i8253; }
    virtual StringView model() const override { return "i8254"sv; }
    virtual size_t ticks_per_second() const override;

    virtual bool is_periodic() const override { return m_periodic; }
    virtual bool is_periodic_capable() const override { return true; }
    virtual void set_periodic() override;
    virtual void set_non_periodic() override;
    virtual void disable() override { }

    virtual void reset_to_default_ticks_per_second() override;
    virtual bool try_to_set_frequency(size_t frequency) override;
    virtual bool is_capable_of_frequency(size_t frequency) const override;
    virtual size_t calculate_nearest_possible_frequency(size_t frequency) const override;

private:
    explicit PIT(Function<void(const RegisterState&)>);
    bool m_periodic { true };
};
}
