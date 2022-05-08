#!/usr/bin/env python
# -*- coding: utf-8 -*-

"""
Build rules for the makeprojects suite of build tools.

This file is parsed by the cleanme, buildme, rebuildme and makeprojects
command line tools to clean, build and generate project files.

When any of these tools are invoked, this file is loaded and parsed to
determine special rules on how to handle building the code and / or data.
"""

# pylint: disable=unused-argument

from __future__ import absolute_import, print_function, unicode_literals

import sys
import os

from burger import clean_directories, clean_files, delete_file, \
    is_under_git_control

from makeprojects.enums import PlatformTypes, ProjectTypes, IDETypes

# Check if git is around
_GIT_FOUND = None

# If set to True, ``buildme -r``` will not parse directories in this folder.
BUILDME_NO_RECURSE = None

# ``buildme``` will build these files and folders first.
BUILDME_DEPENDENCIES = []

# If set to True, ``cleanme -r``` will not parse directories in this folder.
CLEANME_NO_RECURSE = True

# ``cleanme`` will clean the listed folders using their rules before cleaning.
# this folder.
CLEANME_DEPENDENCIES = []

########################################


def is_git(working_directory):
    """
    Detect if perforce or git is source control
    """

    global _GIT_FOUND

    if _GIT_FOUND is None:
        _GIT_FOUND = is_under_git_control(working_directory)
    return _GIT_FOUND

########################################


def prebuild(working_directory, configuration):
    """
    Perform actions before building any IDE based projects.

    This function is called before any IDE or other script is invoked. This is
    perfect for creating headers or other data that the other build projects
    need before being invoked.

    On exit, return 0 for no error, or a non zero error code if there was an
    error to report.

    Args:
        working_directory
            Directory this script resides in.

        configuration
            Configuration to build, ``all`` if no configuration was requested.

    Returns:
        None if not implemented, otherwise an integer error code.
    """
    return None

########################################


def build(working_directory, configuration):
    """
    Build code or data before building IDE project but after data generation.

    Commands like ``makerez`` and ``slicer`` are called before this function is
    invoked so it can assume headers and / or data has been generated before
    issuing custom build commands.

    On exit, return 0 for no error, or a non zero error code if there was an
    error to report.

    Args:
        working_directory
            Directory this script resides in.

        configuration
            Configuration to build, ``all`` if no configuration was requested.

    Returns:
        None if not implemented, otherwise an integer error code.
    """
    return None

########################################


def postbuild(working_directory, configuration):
    """
    Issue build commands after all IDE projects have been built.

    This function can assume all other build projects have executed for final
    deployment or cleanup

    On exit, return 0 for no error, or a non zero error code if there was an
    error to report.

    Args:
        working_directory
            Directory this script resides in.

        configuration
            Configuration to build, ``all`` if no configuration was requested.

    Returns:
        None if not implemented, otherwise an integer error code.
    """
    return None

########################################


def do_project(working_directory, project):
    """
    Set the custom attributes for each configuration.

    Args:
        configuration: Configuration to modify.
    """

    # Too many branches
    # Too many statements
    # pylint: disable=R0912,R0915

    project.solution.perforce = not is_git(working_directory)
    project.source_folders_list = ('./source', './source/windows')


########################################


def rules(command, working_directory, **kargs):
    """
    Main entry point for build_rules.py.

    When ``makeprojects``, ``cleanme``, or ``buildme`` is executed, they will
    call this function to perform the actions required for build customization.

    The parameter ``working_directory`` is required, and if it has no default
    parameter, this function will only be called with the folder that this
    file resides in. If there is a default parameter of ``None``, it will be
    called with any folder that it is invoked on. If the default parameter is a
    directory, this function will only be called if that directory is desired.

    The optional parameter of ``root``` alerts the tool if subsequent processing
    of other ``build_rules.py`` files are needed or if set to have a default
    parameter of ``True``, processing will end once the calls to this
    ``rules()`` function are completed.

    Commands are 'build', 'clean', 'prebuild', 'postbuild', 'project',
    'configurations'

    Arg:
        command: Command to execute.
        working_directory: Directory for this function to operate on.
        root: If True, stop execution upon completion of this function
        kargs: Extra arguments specific to each command.
    Return:
        Zero on no error or no action.
    """

    # Too many return statements
    # Unused arguments
    # pylint: disable=R0911,W0613

    # Commands for makeprojects.
    if command == 'project_settings':
        # Return the settings for a specific project
        do_project(working_directory, project=kargs['project'])

    return 0

########################################


def clean(working_directory):
    """
    Delete temporary files.

    This function is called by ``cleanme`` to remove temporary files.

    On exit, return 0 for no error, or a non zero error code if there was an
    error to report.

    Args:
        working_directory
            Directory this script resides in.

    Returns:
        None if not implemented, otherwise an integer error code.
    """

    source_directory = os.path.join(working_directory, 'source')
    windows_directory = os.path.join(source_directory, 'windows')

    delete_file(os.path.join(source_directory, 'version.h'))
    delete_file(os.path.join(windows_directory, 'messages.h'))
    delete_file(os.path.join(windows_directory, 'messages.rc'))
    clean_files(windows_directory, ('*.bin',))

    clean_directories(working_directory, ('.vscode',
                                          'appfolder',
                                          'temp',
                                          'ipch',
                                          'bin',
                                          '.vs',
                                          '*_Data',
                                          '* Data',
                                          '__pycache__'))

    clean_files(working_directory, ('.DS_Store',
                                    '*.suo',
                                    '*.user',
                                    '*.ncb',
                                    '*.err',
                                    '*.sdf',
                                    '*.layout.cbTemp',
                                    '*.VC.db',
                                    '*.pyc',
                                    '*.pyo'))

    return 0


# If called as a command line and not a class, perform the build
if __name__ == "__main__":
    sys.exit(prebuild(os.path.dirname(os.path.abspath(__file__)), None))
