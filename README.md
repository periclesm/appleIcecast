# Apple Icecast

**Apple Icecast is libShout compiled as framework for iOS and macOS to be linked in applications in order to broadcast on Icecast servers.** </br>
It relies on xiph.org codecs and includes LAME project's libLame for mp3 encoding. 

The project uses the existing codebase of the open source libraries and simply compiles everything into several and distinct frameworks:
* macIcecast.framework - for 64bit macOS 10.10 (Yosemite) version and later
* iosIcecast.framework - fat binary including both 32 and 64bit versions for iOS 8.0 and later
* iosIcecast64.framework - 64bit only iOS 11.0 and later version

Project contains scripts to generate the iOS framework to be used on the device and will allow the app to be build in the simulator however, due to the lack of direct audio devices on the simulator, broadcasting will not run. To test your app, simply install it on a device.

To compile and create the framework, open the project, select the scheme (masOS or iOS fat/64bit) and run it. </br>
In *Build/Products* subfolder, you will find a *Release* folder (macOS) or *Universal* folder (iOS) containing the compiled framework.</br>
Copy the framework in your project and import it in your target.

_All product frameworks are built with **Release** configuration and are ready for your app._

## Download
In the *Frameworks* folder, there are pre-compiled versions for each platform, ready to be imported into projects.</br>
You can also select and download per platform from [here](https://github.com/periclesm/appleIcecast/releases)

## State of Union:
All open source codecs are up-to-date as of 31/12/2019 with the code found on their respective repositories.</br>
Future updates on sources will be released.

## Performance 
There have been several broadcast tests to check performance. On a specific testing usecase (90 min broadcast at 128kbps mp3 on an iPhone X device):
* Battery consumed during the broadcast session was less than 20%,
* No extreme memory or cpu usage observed,
* Broadcast continues with the app in the background and display off (thus saving more battery life),
* No buffering caused by network traffic (wifi or cellular under normal conditions -- clarification: don't broadcast on a bitrate higher than the max bitrate of the Icecast mountpoint),
* Data usage (for mobile data connections) varies depending on the broadcast bitrate.

Goes without saying that during broadcast the device should not be used for any other purpose that may disrupt the availability of audio devices.

## Cookies ;)
When broadcasting from an iPhone or an iPad, it is expected that the audio source is the device microphone.</br>
There are cables (XLR/RCA/TRS (_aka Audio Jack) connector to Lightning_) connecting an input source (ex. mixing console) with an iPhone or iPad. In this case, microphone converts to a line-in device and input audio can be broadcasted on an Icecast server through the app with the Apple Icecast framework (oh yes, this has also been tested successfully!)

## TODO:
* **Provide working examples (apps) how to use the frameworks (very soon _-ish_)**
* Research for Apple Catalyst version (macOS and iOS XCFramework).
* Implement a new metadata method to broadcast information.
* Add opus codec. 
* Add speex codec.
* Enable AAC encoding and broadcasting.

## Known Issues:
1. App crashes while broadcasting and network device changes (aka wifi is lost/fallback to mobile data or Airplane mode is enabled).
2. ???

## Testing needed: 
* Ogg encoding and broadcasting.
* Vorbis encoding and broadcasting.

## Copyrights:
This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

## Specific Copyrights:
* *Ogg* & *Vorbis* codecs by the [Xiph.org Foundation](http://www.xiph.org) (Github source repos: [Ogg](https://github.com/xiph/ogg) and [Vorbis](https://github.com/xiph/vorbis))
* *libShout* by the Icecast team and Philipp Schafft (Github source repo: [Icecast-libshout](https://github.com/xiph/Icecast-libshout))
* *libLAME* by [the LAME Project](https://lame.sourceforge.io) (Sourceforge: [link](https://sourceforge.net/projects/lame))