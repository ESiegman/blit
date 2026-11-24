// A defmt firmware for QEMU's mps2-an385 (tests/rtt.sh, built by build.sh):
// one log at each level and a println, each with a target timestamp (a
// counter, 125 us a log), over semihosting (defmt-semihosting writes the
// frames to the ":tt" handle), then a semihosting exit.
#![no_std]
#![no_main]

use core::sync::atomic::{AtomicU32, Ordering};
use cortex_m_rt::entry;
use defmt_semihosting as _;

static TICKS: AtomicU32 = AtomicU32::new(0);
defmt::timestamp!("{=u32:us}", TICKS.fetch_add(125, Ordering::Relaxed));

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    loop {}
}

#[entry]
fn main() -> ! {
    defmt::trace!("clock tree set up");
    defmt::debug!("adc offset {=i16}", -12i16);
    defmt::info!("motor controller ready, {=u8} phases", 3u8);
    defmt::warn!("supply at {=f32} V, below 3.2", 3.05f32);
    defmt::error!("over-current on phase {=u8}: {=u16} mA", 2u8, 1840u16);
    defmt::println!("done");
    cortex_m_semihosting::debug::exit(cortex_m_semihosting::debug::EXIT_SUCCESS);
    loop {}
}
