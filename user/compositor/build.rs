use std::{env, path::PathBuf, process::Command};

fn main() {
    let root = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap()).join("../..");
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    let mut objects = Vec::new();
    for (name, source) in [
        ("timezone", "user/timezone.c"),
        ("bridge", "user/compositor/timezone_bridge.c"),
    ] {
        let object = out.join(format!("{name}.o"));
        let status = Command::new("gcc")
            .current_dir(&root)
            .args([
                "-std=c11",
                "-Os",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-ffreestanding",
                "-fno-builtin",
                "-fno-pie",
                "-fno-stack-protector",
                "-mno-red-zone",
                "-mno-sse",
                "-mno-mmx",
                "-msoft-float",
                "-fstack-usage",
                "-Wframe-larger-than=1024",
                "-Isrc/include",
                "-Isrc/fs",
                "-Isrc/kernel",
                "-Isrc/mm",
                "-Isrc/arch/x86_64",
                "-c",
                source,
                "-o",
            ])
            .arg(&object)
            .status()
            .expect("gcc for shared timezone engine");
        assert!(status.success(), "timezone adapter compilation failed");
        objects.push(object);
        println!("cargo:rerun-if-changed={}", root.join(source).display());
    }
    assert!(
        Command::new("ar")
            .arg("crs")
            .arg(out.join("libdesktop_time.a"))
            .args(&objects)
            .status()
            .unwrap()
            .success()
    );
    for source in [
        "user/timezone.h",
        "user/tools/timezone_io.c",
        "user/tools/common.h",
        "src/include/syscall_abi.h",
        "src/include/netctl_abi.h",
        "src/include/wifi_abi.h",
        "src/fs/vfs.h",
    ] {
        println!("cargo:rerun-if-changed={}", root.join(source).display());
    }
    println!("cargo:rustc-link-search=native={}", out.display());
    println!("cargo:rustc-link-lib=static=desktop_time");
}
