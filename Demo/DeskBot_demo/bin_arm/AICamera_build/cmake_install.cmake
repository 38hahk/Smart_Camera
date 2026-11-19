# Install script for directory: /home/book/Echo-Mate/Demo/yolov5_demo/cpp

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/home/book/Echo-Mate/Demo/yolov5_demo/cpp/rv1106_yolov5_demo")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "1")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "TRUE")
endif()

# Set default install directory permissions.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/home/book/Echo-Mate/SDK/rv1106-sdk/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}/home/book/Echo-Mate/Demo/yolov5_demo/cpp/rv1106_yolov5_demo/rknn_yolov5_demo" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}/home/book/Echo-Mate/Demo/yolov5_demo/cpp/rv1106_yolov5_demo/rknn_yolov5_demo")
    file(RPATH_CHECK
         FILE "$ENV{DESTDIR}/home/book/Echo-Mate/Demo/yolov5_demo/cpp/rv1106_yolov5_demo/rknn_yolov5_demo"
         RPATH "$ORIGIN/lib")
  endif()
  list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
   "/home/book/Echo-Mate/Demo/yolov5_demo/cpp/rv1106_yolov5_demo/rknn_yolov5_demo")
  if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  file(INSTALL DESTINATION "/home/book/Echo-Mate/Demo/yolov5_demo/cpp/rv1106_yolov5_demo" TYPE EXECUTABLE FILES "/home/book/Echo-Mate/Demo/DeskBot_demo/bin_arm/AICamera_build/rknn_yolov5_demo")
  if(EXISTS "$ENV{DESTDIR}/home/book/Echo-Mate/Demo/yolov5_demo/cpp/rv1106_yolov5_demo/rknn_yolov5_demo" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}/home/book/Echo-Mate/Demo/yolov5_demo/cpp/rv1106_yolov5_demo/rknn_yolov5_demo")
    file(RPATH_CHANGE
         FILE "$ENV{DESTDIR}/home/book/Echo-Mate/Demo/yolov5_demo/cpp/rv1106_yolov5_demo/rknn_yolov5_demo"
         OLD_RPATH "/home/book/Echo-Mate/Demo/yolov5_demo/cpp/3rdparty/rknpu2/Linux/armhf-uclibc:"
         NEW_RPATH "$ORIGIN/lib")
    if(CMAKE_INSTALL_DO_STRIP)
      execute_process(COMMAND "/home/book/Echo-Mate/SDK/rv1106-sdk/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-strip" "$ENV{DESTDIR}/home/book/Echo-Mate/Demo/yolov5_demo/cpp/rv1106_yolov5_demo/rknn_yolov5_demo")
    endif()
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  include("/home/book/Echo-Mate/Demo/DeskBot_demo/bin_arm/AICamera_build/CMakeFiles/rknn_yolov5_demo.dir/install-cxx-module-bmi-noconfig.cmake" OPTIONAL)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
   "/home/book/Echo-Mate/Demo/yolov5_demo/cpp/rv1106_yolov5_demo/model/bus.jpg")
  if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  file(INSTALL DESTINATION "/home/book/Echo-Mate/Demo/yolov5_demo/cpp/rv1106_yolov5_demo/model" TYPE FILE FILES "/home/book/Echo-Mate/Demo/yolov5_demo/cpp/../model/bus.jpg")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
   "/home/book/Echo-Mate/Demo/yolov5_demo/cpp/rv1106_yolov5_demo/model/coco_80_labels_list.txt")
  if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  file(INSTALL DESTINATION "/home/book/Echo-Mate/Demo/yolov5_demo/cpp/rv1106_yolov5_demo/model" TYPE FILE FILES "/home/book/Echo-Mate/Demo/yolov5_demo/cpp/../model/coco_80_labels_list.txt")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
   "/home/book/Echo-Mate/Demo/yolov5_demo/cpp/rv1106_yolov5_demo/model/yolov5.rknn")
  if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  file(INSTALL DESTINATION "/home/book/Echo-Mate/Demo/yolov5_demo/cpp/rv1106_yolov5_demo/model" TYPE FILE FILES "/home/book/Echo-Mate/Demo/yolov5_demo/cpp/../model/yolov5.rknn")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for each subdirectory.
  include("/home/book/Echo-Mate/Demo/DeskBot_demo/bin_arm/AICamera_build/3rdparty.out/cmake_install.cmake")
  include("/home/book/Echo-Mate/Demo/DeskBot_demo/bin_arm/AICamera_build/utils.out/cmake_install.cmake")

endif()

