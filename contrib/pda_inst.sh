#!/bin/bash

set -e
set -u

PDA_VERSION="11.4.7"
USER_NAME=`id -u -n`

getdeps()
{
  apt-get install libpci-dev gcc libtool libtool-bin libkmod-dev linux-headers-`uname -r`
}

install()
{
  rm -rf /tmp/pda-$1*

  cd /tmp
  wget https://github.com/cbm-fles/pda/archive/$1.tar.gz
  tar -xf $1.tar.gz
  rm -f $1.tar.gz

  cd /tmp/pda-$1
  mkdir -p /opt/pda/
  ./configure --debug=true --prefix=/opt/pda/$1/
  make install

  cd /tmp/pda-$1/patches/linux_uio/
  make install

  cd /tmp
  rm -rf /tmp/pda-$1
}

patchmodulelist()
{
  if [ -z "`cat /etc/modules | grep uio_pci_dma`" ]
  then
    echo "# Added by pda install script" >> /etc/modules
    echo "uio_pci_dma" >> /etc/modules
    echo "Module added"
  fi
  modprobe uio_pci_dma
}

add_systemgroup()
{
    if ! grep -q -E "^pda" /etc/group; then
        echo "Creating system group 'pda'"
        addgroup --system pda
    fi
}

clean_old_install()
{
    if [ -f /etc/pda_sysfs.sh ]
    then
        echo "Removing old files"
        rm /etc/pda_sysfs.sh
    fi
}

CTRL="\e[1m\e[32m"

if [ "root" = "$USER_NAME" ]
then
    echo "Now running as $USER_NAME"

    getdeps
    clean_old_install
    add_systemgroup
    install $PDA_VERSION
    patchmodulelist

    echo -e "$CTRL YOU MUST RESTART UDEV NOW. A REBOOT WILL DO THE JOB IF IT'S UNCERTAIN WHAT TO DO!"
    echo -e "$CTRL Please add all flib users to group 'pda', e.g., 'usermod -a -G pda <user>'"
else
    echo "Running as user!"
    sudo $0
fi
