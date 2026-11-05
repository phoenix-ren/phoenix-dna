# SPDX-FileCopyrightText: 2026 Phoenix DNA contributors
# SPDX-License-Identifier: BSD-2-Clause

# The Python agent daemon: the `phoenix_dna` package, its two launchers, the
# systemd *user* unit and the D-Bus activation file.
#
# The daemon is not compiled. It is a stdlib-only Python package that uses the
# distribution's python3-dbus and python3-gobject (see agent/README.md). It is
# installed into a private directory instead of site-packages, so it cannot
# collide with the distribution's Python packaging; the launchers put that
# directory on PYTHONPATH and hand over to the interpreter.

find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)

set(PHOENIX_AGENT_DATADIR "${KDE_INSTALL_DATADIR}/phoenix-dna/agent")
set(PHOENIX_AGENT_FULL_DATADIR "${KDE_INSTALL_FULL_DATADIR}/phoenix-dna/agent")

# The interpreter the installed launchers run. It defaults to the one CMake
# found, which is the right answer for a source install; a distribution can
# pass -DPHOENIX_AGENT_PYTHON=/usr/bin/python3 so the package does not depend
# on the build host's exact minor version.
set(PHOENIX_AGENT_PYTHON "${Python3_EXECUTABLE}" CACHE FILEPATH
    "Python interpreter used by the installed Phoenix DNA agent launchers")

install(DIRECTORY phoenix_dna
        DESTINATION "${PHOENIX_AGENT_DATADIR}"
        FILES_MATCHING
            PATTERN "*.py"
            PATTERN "*.pyc" EXCLUDE
            PATTERN "__pycache__" EXCLUDE)

# The README is what the unit's Documentation= points at; the example file is
# the one the README tells the user to copy.
install(FILES README.md config-example.toml
        DESTINATION "${PHOENIX_AGENT_DATADIR}")

# Two tiny shell launchers. One template, two module names: the daemon and the
# terminal REPL. Installing them gives packaged systems the same commands the
# README documents for a checkout.
set(PHOENIX_AGENT_MODULE "phoenix_dna")
configure_file(systemd/phoenix-dna.in
               "${CMAKE_CURRENT_BINARY_DIR}/phoenix-dna-agent" @ONLY)
set(PHOENIX_AGENT_MODULE "phoenix_dna.repl")
configure_file(systemd/phoenix-dna.in
               "${CMAKE_CURRENT_BINARY_DIR}/phoenix-dna-repl" @ONLY)
install(PROGRAMS
            "${CMAKE_CURRENT_BINARY_DIR}/phoenix-dna-agent"
            "${CMAKE_CURRENT_BINARY_DIR}/phoenix-dna-repl"
        DESTINATION ${KDE_INSTALL_BINDIR})

# The systemd user unit is the same template the checkout installer fills in,
# so there is one definition of the service. A packaged install points
# ExecStart at the launcher and WorkingDirectory at the private package
# directory.
set(AGENT_EXEC "${KDE_INSTALL_FULL_BINDIR}/phoenix-dna-agent")
set(AGENT_DIR "${PHOENIX_AGENT_FULL_DATADIR}")
configure_file(systemd/phoenix.service.in
               "${CMAKE_CURRENT_BINARY_DIR}/phoenix.service" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/phoenix.service"
        DESTINATION ${KDE_INSTALL_SYSTEMDUSERUNITDIR})

# D-Bus activation: the plasmoid's first method call starts the daemon even
# when the user unit has not been enabled. SystemdService= lets systemd own
# the start, so a broken start is a failed unit in the journal rather than an
# opaque activation failure. The unit is still enabled at login on systems
# whose preset policy says so.
set(PHOENIX_DBUS_SERVICE_NAME "dna.phoenix.Agent")
set(PHOENIX_DBUS_SERVICE_EXEC "${KDE_INSTALL_FULL_BINDIR}/phoenix-dna-agent")
set(PHOENIX_DBUS_SYSTEMD_SERVICE "phoenix.service")
configure_file(systemd/dna.phoenix.Agent.service.in
               "${CMAKE_CURRENT_BINARY_DIR}/dna.phoenix.Agent.service" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/dna.phoenix.Agent.service"
        DESTINATION ${KDE_INSTALL_DBUSSERVICEDIR})
