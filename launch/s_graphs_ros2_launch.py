# Copyright 2021 Intelligent Robotics Lab
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


import os

from ament_index_python.packages import get_package_share_directory

from launch import LaunchDescription
from launch_ros.actions import Node


def generate_launch_description():

    pkg_dir = get_package_share_directory('s_graphs')
    param_file = os.path.join(pkg_dir, 'config', 'hdl_prefiltering.yaml')

    param_reader_cmd = Node(package='s_graphs', executable='s_graphs_prefiltering_node', parameters=[param_file], output='screen', remappings=[('velodyne_points', '/platform/velodyne_points'),
                                                                                                                                               ('imu/data', '/platform/imu/data')]
                            )

    ld = LaunchDescription()
    ld.add_action(param_reader_cmd)

    return ld