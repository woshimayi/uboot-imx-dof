#!/bin/bash

make distclean 
#make mx6ull_14x14_dof_emmc_defconfig
make mx6ull_14x14_dof_nand_defconfig
make 
