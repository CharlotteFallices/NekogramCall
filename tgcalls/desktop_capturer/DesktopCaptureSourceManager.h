//
//  DesktopCaptureSourceManager.h
//  TgVoipWebrtc
//
//  Created by Mikhail Filimonov on 28.12.2020.
//  Copyright © 2020 Mikhail Filimonov. All rights reserved.
//

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>

#import "DesktopCaptureSource.h"
NS_ASSUME_NONNULL_BEGIN



@interface DesktopCaptureSourceManager : NSObject

-(instancetype)init_s;
-(instancetype)init_w;
-(NSArray<DesktopCaptureSource *> *)list;

#ifndef WEBRTC_APP_TDESKTOP
-(NSView *)createForScope:(DesktopCaptureSourceScope *)scope;
#endif // WEBRTC_APP_TDESKTOP

-(void)start:(DesktopCaptureSourceScope *)scope;
-(void)stop:(DesktopCaptureSourceScope *)scope;

@end

NS_ASSUME_NONNULL_END
