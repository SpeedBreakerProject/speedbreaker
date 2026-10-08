// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See ios_files.h.
//
// Manual reference counting: the runtime is built without -fobjc-arc, so
// every alloc and retain here has its release.
#include "ios_files.h"

#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <cerrno>
#include <cstdio>
#include <map>
#include <mutex>
#include <string_view>
#include <utility>

#include <dirent.h>
#include <sys/stat.h>

using platform::ios::FileAccess;
using platform::ios::PickedFile;

@interface SBDiscImagePickerDelegate : NSObject <UIDocumentPickerDelegate>
{
@public
    std::function<void(std::optional<PickedFile>)> done;
    bool folder;
}
@end

namespace
{
    // Picked files whose grant is held (retained URLs), by FileAccess.
    std::mutex s_accessMutex;
    std::map<FileAccess, NSURL*> s_held;
    FileAccess s_lastAccess = 0;

    // The picker on screen and its delegate, which the picker holds only
    // weakly: owned here until it answers or a new Browse replaces it.
    UIDocumentPickerViewController* s_picker = nil;
    SBDiscImagePickerDelegate* s_delegate = nil;
    // The last picker that answered or was replaced, kept until UIKit has it
    // off screen (IsPickerShown): it may still be sliding away.
    UIDocumentPickerViewController* s_leaving = nil;

    FileAccess Hold(NSURL* url)
    {
        // NO for a file in SpeedBreaker's own folder, which needs no grant.
        if (![url startAccessingSecurityScopedResource])
            return 0;
        std::lock_guard lock(s_accessMutex);
        s_held[++s_lastAccess] = [url retain];
        return s_lastAccess;
    }

    // Asks the cloud service `path` belongs to for its data. False if
    // nothing will download it (not a cloud item, or the service said no).
    bool StartDownload(const std::string& path, bool directory)
    {
        @autoreleasepool
        {
            NSURL* url = [NSURL fileURLWithFileSystemRepresentation:path.c_str() isDirectory:directory relativeToURL:nil];
            NSError* error = nil;
            if ([[NSFileManager defaultManager] startDownloadingUbiquitousItemAtURL:url error:&error])
                return true;
            fprintf(stderr, "[files] couldn't start downloading %s: %s\n", path.c_str(),
                error ? error.localizedDescription.UTF8String : "unknown error");
            return false;
        }
    }

    // A cloud service (iCloud Drive, or another app's through the Files app)
    // lists files it hasn't downloaded, and the picker hands them over as
    // they are. Most are dataless files, whose data comes only when
    // something reads them: the check would wait for all 7 GB with nothing
    // to say why. Some services, and older iCloud Drive, have no file there
    // at all yet (iCloud's stand-in is a hidden .<name>.icloud beside it), so
    // the check would say it doesn't exist. lstat tells which without
    // starting a download, as a read would.
    bool FileNotHere(NSURL* url)
    {
        struct stat st;
        if (lstat(url.fileSystemRepresentation, &st) != 0)
            return errno == ENOENT;
        if (st.st_flags & SF_DATALESS)
            return true;
        // iCloud's own word for it. Downloaded (an older version) is here, and
        // reads at once; only NotDownloaded isn't.
        NSString* status = nil;
        return [url getResourceValue:&status forKey:NSURLUbiquitousItemDownloadingStatusKey error:nil] && status &&
            [status isEqualToString:NSURLUbiquitousItemDownloadingStatusNotDownloaded];
    }

    // What a picked folder holds that isn't here, and how many of those
    // downloads were asked for.
    struct Missing
    {
        int seen = 0;
        int missing = 0;
        int started = 0;

        void Add(const std::string& path, bool directory)
        {
            missing++;
            // Enough for a whole disc (about 50 files); past that, the Files
            // app's Download Now on the folder.
            if (started < 64)
                started += StartDownload(path, directory);
        }
    };

    // The same for everything in a picked folder, all the way down, by lstat
    // alone. Doesn't go into a folder that isn't here itself, since listing
    // it would wait for its download; links aren't followed. An extracted
    // disc is about 50 files, so it stops looking after a few thousand (the
    // wrong folder, which the check will say).
    void FindNotHere(const std::string& dir, int depth, Missing& out)
    {
        DIR* d = opendir(dir.c_str());
        if (!d)
            return;
        while (dirent* e = readdir(d))
        {
            if (++out.seen > 4096)
                break;
            std::string_view name = e->d_name;
            if (name == "." || name == "..")
                continue;
            if (name.size() > 8 && name.front() == '.' && name.ends_with(".icloud"))
            {
                // .<name>.icloud: <name> isn't downloaded; it's asked for by its own path.
                out.Add(dir + "/" + std::string(name.substr(1, name.size() - 8)), false);
                continue;
            }
            std::string path = dir + "/" + std::string(name);
            struct stat st;
            if (lstat(path.c_str(), &st) != 0 || S_ISLNK(st.st_mode))
                continue;
            if (st.st_flags & SF_DATALESS)
                out.Add(path, S_ISDIR(st.st_mode));
            else if (S_ISDIR(st.st_mode) && depth < 16)
                FindNotHere(path, depth + 1, out);
        }
        closedir(d);
    }

    // Whether a picked item isn't on the device; if so, asks for it and
    // returns what to tell the player.
    std::string CheckHere(NSURL* url, bool folder)
    {
        std::string path = url.fileSystemRepresentation;
        Missing found;
        if (FileNotHere(url))
            found.Add(path, folder);
        else if (folder)
            FindNotHere(path, 0, found);
        if (!found.missing)
            return {};
        fprintf(stderr, "[files] %s: %d item%s not on this device yet, %d download%s asked for\n", path.c_str(),
            found.missing, found.missing == 1 ? "" : "s", found.started, found.started == 1 ? "" : "s");

        const char* device = UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad ? "iPad" : "iPhone";
        NSString* last = url.lastPathComponent;
        std::string name = last.length ? last.UTF8String : path;
        std::string text = (folder ? "Some of " + name : name) + " isn't on this " + device + " yet";
        const char* it = folder ? "the folder" : "it";
        if (found.started)
        {
            // iCloud Drive's files live in Mobile Documents; other services'
            // in their own app's storage.
            bool icloud = path.find("/Mobile Documents/") != std::string::npos;
            return text + ": it's downloading" + (icloud ? " from iCloud Drive" : "") + " (or open " + it +
                " in the Files app and choose Download Now). When it's done, choose Browse again.";
        }
        return text + ". Open " + it + " in the Files app and choose Download Now, and when it's done, choose Browse again.";
    }

    // Shown, or on its way on or off screen.
    bool OnScreen(UIViewController* controller)
    {
        return controller && (controller.presentingViewController || controller.isBeingPresented || controller.isBeingDismissed);
    }

    // Disconnects the picker on screen, which then never answers.
    // Autoreleased rather than released: this runs inside its delegate's
    // own callback.
    void Forget()
    {
        s_picker.delegate = nil;
        if (s_picker)
        {
            [s_leaving autorelease];
            s_leaving = [s_picker retain];
        }
        [s_picker autorelease];
        [s_delegate autorelease];
        s_picker = nil;
        s_delegate = nil;
    }

    void Answer(SBDiscImagePickerDelegate* delegate, std::optional<PickedFile> picked)
    {
        if (delegate != s_delegate)
        {
            // A replaced picker (disconnected, so this shouldn't happen).
            if (picked)
                platform::ios::ReleaseFileAccess(picked->access);
            return;
        }
        std::function<void(std::optional<PickedFile>)> done = std::move(delegate->done);
        Forget();
        if (done)
            done(std::move(picked));
    }
}

@implementation SBDiscImagePickerDelegate

- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls
{
    std::optional<PickedFile> picked;
    NSURL* url = urls.firstObject;
    if (url && url.fileURL)
    {
        picked = PickedFile{ url.fileSystemRepresentation, Hold(url) };
        fprintf(stderr, "[files] picked %s (%s)\n", picked->path.c_str(),
            picked->access ? "reading it under the Files app's grant" : "no grant needed");
        // Looked at under the grant; not read until it's all here.
        picked->notHere = CheckHere(url, folder);
        if (!picked->notHere.empty())
            platform::ios::ReleaseFileAccess(std::exchange(picked->access, 0));
    }
    Answer(self, std::move(picked));
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller
{
    Answer(self, std::nullopt);
}

@end

namespace platform::ios
{
    bool PickDiscImage(SDL_Window* window, bool folder, std::function<void(std::optional<PickedFile>)> done)
    {
        @autoreleasepool
        {
            UIWindow* uiWindow = window ? static_cast<UIWindow*>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr)) : nil;
            UIViewController* root = uiWindow.rootViewController;
            if (!root)
            {
                fprintf(stderr, "[files] no view controller to show the document picker from\n");
                return false;
            }

            // An image's type, or any file: the picker greys out what the
            // types don't cover, and a USB drive or a cloud service may not
            // know .iso (the check says what a file really is).
            NSMutableArray<UTType*>* types = [NSMutableArray array];
            if (folder)
                [types addObject:UTTypeFolder];
            else
            {
                if (UTType* iso = [UTType typeWithIdentifier:@"public.iso-image"])
                    [types addObject:iso];
                [types addObject:UTTypeData];
            }
            // asCopy:NO opens the file where it is, under a grant; a copy
            // would put another 7 GB on the device first.
            UIDocumentPickerViewController* picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types
                asCopy:NO];
            picker.allowsMultipleSelection = NO;
            picker.shouldShowFileExtensions = YES;
            SBDiscImagePickerDelegate* delegate = [[SBDiscImagePickerDelegate alloc] init];
            delegate->done = std::move(done);
            delegate->folder = folder;
            picker.delegate = delegate;

            // A new Browse replaces a picker still open.
            UIDocumentPickerViewController* previous = [s_picker retain];
            Forget();
            s_picker = picker;
            s_delegate = delegate;
            void (^present)(void) = ^{
                if (picker != s_picker)
                    return;  // replaced again before it was shown
                UIViewController* top = root;
                while (top.presentedViewController && !top.presentedViewController.isBeingDismissed)
                    top = top.presentedViewController;
                [top presentViewController:picker animated:YES completion:nil];
            };
            if (previous.presentingViewController)
                [previous dismissViewControllerAnimated:NO completion:present];
            else
                present();
            [previous release];
            return true;
        }
    }

    bool IsPickerShown()
    {
        @autoreleasepool
        {
            if (s_leaving && !OnScreen(s_leaving))
            {
                [s_leaving release];
                s_leaving = nil;
            }
            return OnScreen(s_picker) || s_leaving;
        }
    }

    void ReleaseFileAccess(FileAccess access)
    {
        NSURL* url = nil;
        {
            std::lock_guard lock(s_accessMutex);
            auto it = s_held.find(access);
            if (it == s_held.end())
                return;
            url = it->second;
            s_held.erase(it);
        }
        [url stopAccessingSecurityScopedResource];
        [url release];
    }

    bool ExcludeFromBackup(const std::filesystem::path& dir)
    {
        @autoreleasepool
        {
            NSURL* url = [NSURL fileURLWithFileSystemRepresentation:dir.c_str() isDirectory:YES relativeToURL:nil];
            NSError* error = nil;
            if ([url setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:&error])
                return true;
            fprintf(stderr, "[files] couldn't exclude %s from backups: %s\n", dir.c_str(),
                error ? error.localizedDescription.UTF8String : "unknown error");
            return false;
        }
    }

    std::optional<uint64_t> AvailableCapacityForImportantUsage(const std::filesystem::path& path)
    {
        @autoreleasepool
        {
            // A new URL each time: an NSURL caches the values it was asked for.
            NSURL* url = [NSURL fileURLWithFileSystemRepresentation:path.c_str() isDirectory:YES relativeToURL:nil];
            NSNumber* value = nil;
            if (![url getResourceValue:&value forKey:NSURLVolumeAvailableCapacityForImportantUsageKey error:nil] || !value)
                return std::nullopt;
            // 0 is also what it says when it can't tell (seen on some
            // releases); the caller's statvfs knows a full disk as well.
            long long bytes = value.longLongValue;
            if (bytes <= 0)
                return std::nullopt;
            return uint64_t(bytes);
        }
    }
}
