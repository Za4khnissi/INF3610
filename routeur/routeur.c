# Installing Microsoft DirectX End-User Runtime June2010

#{
#  "builders": [
#    {
#      "type": "qemu",
#      "iso_url": "path/to/your/iso",
#      "iso_checksum": "checksum_value",
#      "output_directory": "output_directory",
#      "disk_size": "5000M",
#      "format": "qcow2",
#      "accelerator": "kvm",
#      "net_device": "virtio-net",
#      "disk_interface": "virtio",
#      "boot_wait": "10s",
#      "boot_command": [
#        "<tab> text ks=http://{{ .HTTPIP }}:{{ .HTTPPort }}/kickstart.cfg<enter><wait>"
#      ]
#    }
#  ]
#}

packer {
  required_plugins {
    qemu = {
      version = ">= 1.0.0"
      source  = "github.com/hashicorp/qemu"
    }
  }
}

source "qemu" "windows" {
  iso_url      = "ISO/Windows10LTSC2019-USB-4.5.10.0.iso"
#  iso_url      = "ISO/en_windows_10_iot_enterprise_ltsc_2019_x64_dvd_a1aa819f.iso"
  iso_checksum = "none"
#  floppy_dirs = ["C:/Users/cadnot/Downloads/ItSas35_Windows10_11_Windows_Server_2019_2022_2025_P32/ItSas35_Windows10_11_Windows_Server_2019_2022_2025_P32/Win10_Client_RS5_LTSC_f18_x64"]
  output_directory = "output-windows"
  disk_size = "125G"
  format = "qcow2"
#  format = "raw"

  # cd_files = ["C:/Users/cadnot/Downloads/Win10_Client_RS5_LTSC_f18_x64/itsas35.cat", "C:/Users/cadnot/Downloads/Win10_Client_RS5_LTSC_f18_x64/itsas35.inf","C:/Users/cadnot/Downloads/Win10_Client_RS5_LTSC_f18_x64/itsas35.sys","C:/Users/cadnot/Downloads/Win10_Client_RS5_LTSC_f18_x64/lsinodrv.inf"]
  # cd_label = "cidata"

  machine_type = "q35"
  accelerator = "tcg"
  memory = "8192"
  sockets = "2"
  cores = "2"

  display = "sdl"
  qemuargs = [
    [ "-audiodev", "none,id=none" ],
  ]

  disk_interface = "virtio"
  net_device = "virtio-net"

#  headless = false

  communicator = "ssh"
  ssh_username = "administrator"
  ssh_password = "4CAE_simRecovery"
  ssh_timeout = "4h"

  boot_wait = "4m"
  boot_command = [
    "<enter><wait55>",
    "<tab><spacebar><wait5><enter><wait5>",
    "<enter><wait5>",
    "<enter><wait15m>",
    "<wait150m>",
  ]

  shutdown_command = "shutdown /s /t 10 /f /d p:4:1 /c \"Packer Shutdown\""
}

build {
  sources = ["source.qemu.windows"]

  provisioner "shell" {
    inline = [
      "echo Installing Windows...",
      "powershell.exe -ExecutionPolicy Bypass -File C:\\path\\to\\setup.ps1"
    ]
  }
}
