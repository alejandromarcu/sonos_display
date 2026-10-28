#pragma once

// Web page for setting up the remote's slots (see slots.h), at http://<HOSTNAME>.local.

void webBegin();

// Handles pending requests; call from loop().
void webPoll();
