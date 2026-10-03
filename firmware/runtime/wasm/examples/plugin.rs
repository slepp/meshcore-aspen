// SPDX-License-Identifier: Apache-2.0
#![no_std]
#![no_main]
#[path = "../sdk/meshcore.rs"]
mod mc;
use core::panic::PanicInfo;
#[panic_handler]
fn panic(_: &PanicInfo) -> ! {
    core::arch::wasm32::unreachable()
}
static mut BUFFER: [u8; 151] = [0; 151];

#[unsafe(no_mangle)]
pub unsafe extern "C" fn mc_init() -> i32 {
    unsafe {
        #[cfg(arithmetic)]
        let (name, schema, permission): (&[u8], &[u8], u32) = (
            b"radd",
            b"a:int:-1000000:1000000,b:int:-1000000:1000000",
            mc::PUBLIC,
        );
        #[cfg(notes)]
        let (name, schema, permission): (&[u8], &[u8], u32) =
            (b"rnote", b"text?:text:150", mc::PRIVATE);
        #[cfg(rpc)]
        let (name, schema, permission): (&[u8], &[u8], u32) =
            (b"recho", b"text:text:150", mc::HOME);
        mc::command(
            1,
            name.as_ptr(),
            name.len() as u32,
            schema.as_ptr(),
            schema.len() as u32,
            b"Portable Rust example".as_ptr(),
            21,
            permission,
        );
        1
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mc_start(job: u32, _: u32) -> i32 {
    unsafe {
        let buffer = (&raw mut BUFFER).cast::<u8>();
        #[cfg(arithmetic)]
        {
            let (mut a, mut b) = (0i32, 0i32);
            mc::read(job, mc::ARG0, (&raw mut a).cast(), 4);
            mc::read(job, mc::ARG1, (&raw mut b).cast(), 4);
            let sum = a + b;
            let mut number = sum.unsigned_abs();
            let mut n = 0;
            loop {
                *buffer.add(n) = b'0' + (number % 10) as u8;
                n += 1;
                number /= 10;
                if number == 0 {
                    break;
                }
            }
            if sum < 0 {
                *buffer.add(n) = b'-';
                n += 1;
            }
            for i in 0..n / 2 {
                core::ptr::swap(buffer.add(i), buffer.add(n - i - 1));
            }
            mc::reply(job, buffer, n as u32);
            mc::DONE
        }
        #[cfg(notes)]
        {
            let n = mc::read(job, mc::ARGUMENTS, buffer, 150);
            mc::io(
                job,
                if n > 0 { mc::PUT } else { mc::GET },
                mc::CALLER,
                b"rust-note".as_ptr(),
                9,
                buffer,
                n as u32,
                0,
            );
            mc::PENDING
        }
        #[cfg(rpc)]
        {
            let n = mc::read(job, mc::ARG0, buffer, 150);
            mc::io(
                job,
                mc::RPC,
                mc::CALLER,
                b"echo".as_ptr(),
                4,
                buffer,
                n as u32,
                0,
            );
            mc::PENDING
        }
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mc_resume(job: u32, operation: u32, ok: u32) -> i32 {
    unsafe {
        let buffer = (&raw mut BUFFER).cast::<u8>();
        #[cfg(notes)]
        {
            let (mut kind, mut found) = (0u32, 0u32);
            mc::read(operation, mc::OPERATION_KIND, (&raw mut kind).cast(), 4);
            mc::read(operation, mc::FOUND, (&raw mut found).cast(), 4);
            if ok != 0 && kind == mc::PUT {
                mc::reply(job, b"Note saved".as_ptr(), 10);
                return mc::DONE;
            }
            if ok != 0 && found == 0 {
                mc::reply(job, b"No note saved".as_ptr(), 13);
                return mc::DONE;
            }
        }
        let n = mc::read(
            operation,
            if ok != 0 { mc::VALUE } else { mc::ERROR },
            buffer,
            150,
        );
        if n > 0 {
            mc::reply(job, buffer, n as u32);
        } else {
            mc::reply(job, b"Storage failed".as_ptr(), 14);
        }
        mc::DONE
    }
}
