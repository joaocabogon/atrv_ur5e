#!/usr/bin/env python

from distutils.core import setup
from catkin_pkg.python_setup import generate_distutils_setup

# for your packages to be recognized by python
d = generate_distutils_setup(
 packages=['actions_ros'],
 package_dir={'actions_ros': 'ros/src/actions_ros'}
)

setup(**d)
