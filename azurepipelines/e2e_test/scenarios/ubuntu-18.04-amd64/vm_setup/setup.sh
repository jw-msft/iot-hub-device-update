#!/bin/bash
# -------------------------------------------------------------------------
# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License. See License.txt in the project root for
# license information.
# --------------------------------------------------------------------------
# Setup Script for Test: Ubuntu-18.04-amd64-APT-deployment
#
# Should be run on the Virtual Machine being provisioned for the work.

# This file should be included in an archive that is created at provisioning time
# with the following structure:
#       setup.sh (this file)
#       deviceupdate-package.deb (debian package under test)
#       <other-supporting-files>
# where the archive is named:
#       testsetup.tar.gz
# The archive is manually mounted to the file system at:
#       ~/testsetup.tar.gz
# Once the script has been extracted we'll be running from
# ~ while the script itself is at
#       ~/testsetup/setup.sh
# So we need to localize the path to that.
#
#
# Install the Microsoft APT repository
#

wget https://packages.microsoft.com/config/ubuntu/18.04/multiarch/packages-microsoft-prod.deb -O packages-microsoft-prod.deb
sudo dpkg -i packages-microsoft-prod.deb
rm packages-microsoft-prod.deb

#
# Update the apt repositories
#
sudo apt-get update

#
# Install Device Update Dependencies from APT
#
# Note: If there are other dependencies tht need to be installed via APT or other means they should
# be added here. You might be installing iotedge, another package to setup for a deployment test, or
# anything else.
sudo apt-get install -y deliveryoptimization-plugin-apt

#
# Install the Device Update Artifact Under Test
#
sudo apt-get install -y ./testsetup/deviceupdate-package.deb

#
# Install the du-config.json so the device can be provisioned
#
# Note: In other setup scripts there may be more info that needs to
# go here. For instance you might have the config.toml for IotEdge,
# another kind of diagnostics file, or other kinds of data
# this is the area where such things can be added
sudo cp ./testsetup/du-config.json /etc/adu/du-config.json

#
# Restart the adu-agent.service
#
# Note: We expect that everything should be setup for the deviceupdate agent at this point. Once
# we restart the agent we expect it to be able to boot back up and connect to the IotHub. Otherwise
# this test will be considered a failure.
sudo systemctl restart adu-agent.service