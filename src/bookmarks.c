/* Copyright 2020 Jaakko Keränen <jaakko.keranen@iki.fi>

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. */

#include "bookmarks.h"
#include "visited.h"
#include "gmrequest.h"
#include "app.h"

#include <the_Foundation/file.h>
#include <the_Foundation/hash.h>
#include <the_Foundation/mutex.h>
#include <the_Foundation/path.h>
#include <the_Foundation/regexp.h>
#include <the_Foundation/stringset.h>
#include <the_Foundation/toml.h>

void init_Bookmark(iBookmark *d) {
    init_String(&d->url);
    init_String(&d->title);
    init_String(&d->tags);
    iZap(d->when);
    d->parentId = 0;
    d->order = 0;
}

void deinit_Bookmark(iBookmark *d) {
    deinit_String(&d->tags);
    deinit_String(&d->title);
    deinit_String(&d->url);
}

iBool hasTag_Bookmark(const iBookmark *d, const char *tag) {
    if (!d) return iFalse;
    iRegExp *pattern = new_RegExp(format_CStr("\\b%s\\b", tag), caseSensitive_RegExpOption);
    iRegExpMatch m;
    init_RegExpMatch(&m);
    const iBool found = matchString_RegExp(pattern, &d->tags, &m);
    iRelease(pattern);
    return found;
}

void addTag_Bookmark(iBookmark *d, const char *tag) {
    if (!isEmpty_String(&d->tags)) {
        appendChar_String(&d->tags, ' ');
    }
    appendCStr_String(&d->tags, tag);
}

void removeTag_Bookmark(iBookmark *d, const char *tag) {
    const size_t pos = indexOfCStr_String(&d->tags, tag);
    if (pos != iInvalidPos) {
        remove_Block(&d->tags.chars, pos, strlen(tag));
        trim_String(&d->tags);
    }
}

iDefineTypeConstruction(Bookmark)

static int cmpTimeDescending_Bookmark_(const iBookmark **a, const iBookmark **b) {
    return iCmp(seconds_Time(&(*b)->when), seconds_Time(&(*a)->when));
}

int cmpTitleAscending_Bookmark(const iBookmark **a, const iBookmark **b) {
    return cmpStringCase_String(&(*a)->title, &(*b)->title);
}

iBool filterInsideFolder_Bookmark(void *context, const iBookmark *bm) {
    return hasParent_Bookmark(bm, id_Bookmark(context));
}

/*----------------------------------------------------------------------------------------------*/

static const char *oldFileName_Bookmarks_ = "bookmarks.txt";
static const char *fileName_Bookmarks_    = "bookmarks.ini"; /* since v1.7 (TOML subset) */

struct Impl_Bookmarks {
    iMutex *  mtx;
    int       idEnum;
    iHash     bookmarks; /* bookmark ID is the hash key */
    uint32_t  recentFolderId; /* recently interacted with */
    iPtrArray remoteRequests;   
};

iDefineTypeConstruction(Bookmarks)

void init_Bookmarks(iBookmarks *d) {
    d->mtx = new_Mutex();
    d->idEnum = 0;
    init_Hash(&d->bookmarks);
    d->recentFolderId = 0;
    init_PtrArray(&d->remoteRequests);
}

void deinit_Bookmarks(iBookmarks *d) {
    iForEach(PtrArray, i, &d->remoteRequests) {
        cancel_GmRequest(i.ptr);
        free(userData_Object(i.ptr));
        iRelease(i.ptr);
    }
    deinit_PtrArray(&d->remoteRequests);
    clear_Bookmarks(d);
    deinit_Hash(&d->bookmarks);
    delete_Mutex(d->mtx);
}

void clear_Bookmarks(iBookmarks *d) {
    lock_Mutex(d->mtx);
    iForEach(Hash, i, &d->bookmarks) {
        delete_Bookmark((iBookmark *) i.value);
    }
    clear_Hash(&d->bookmarks);
    d->idEnum = 0;
    unlock_Mutex(d->mtx);
}

static void insertId_Bookmarks_(iBookmarks *d, iBookmark *bookmark, int id) {
    bookmark->node.key = id;
    insert_Hash(&d->bookmarks, &bookmark->node);
}

static void insert_Bookmarks_(iBookmarks *d, iBookmark *bookmark) {
    lock_Mutex(d->mtx);
    insertId_Bookmarks_(d, bookmark, ++d->idEnum);
    unlock_Mutex(d->mtx);
}

static void loadOldFormat_Bookmarks(iBookmarks *d, const char *dirPath) {
    iFile *f = newCStr_File(concatPath_CStr(dirPath, oldFileName_Bookmarks_));
    if (open_File(f, readOnly_FileMode | text_FileMode)) {
        const iRangecc src = range_Block(collect_Block(readAll_File(f)));
        iRangecc line = iNullRange;
        while (nextSplit_Rangecc(src, "\n", &line)) {
            /* Skip empty lines. */ {
                iRangecc ln = line;
                trim_Rangecc(&ln);
                if (isEmpty_Range(&ln)) {
                    continue;
                }
            }
            iBookmark *bm = new_Bookmark();
            bm->icon = strtoul(line.start, NULL, 16);
            line.start += 9;
            char *endPos;
            initSeconds_Time(&bm->when, strtod(line.start, &endPos));
            line.start = skipSpace_CStr(endPos);
            setRange_String(&bm->url, line);
            /* Clean up the URL. */ {
                iUrl parts;
                init_Url(&parts, &bm->url);
                if (isEmpty_Range(&parts.path) && isEmpty_Range(&parts.query)) {
                    appendChar_String(&bm->url, '/');
                }
                stripDefaultUrlPort_String(&bm->url);
                set_String(&bm->url, canonicalUrl_String(&bm->url));
            }
            nextSplit_Rangecc(src, "\n", &line);
            setRange_String(&bm->title, line);
            nextSplit_Rangecc(src, "\n", &line);
            setRange_String(&bm->tags, line);
            insert_Bookmarks_(d, bm);
        }
    }
    iRelease(f);
}

/*----------------------------------------------------------------------------------------------*/

iDeclareType(BookmarkLoader)
    
struct Impl_BookmarkLoader {
    iTomlParser *toml;
    iBookmarks * bookmarks;
    iBookmark *  bm;
};

static void handleTable_BookmarkLoader_(void *context, const iString *table, iBool isStart) {
    iBookmarkLoader *d = context;
    if (isStart) {
        iAssert(!d->bm);
        d->bm = new_Bookmark();
        const int id = toInt_String(table);
        d->bookmarks->idEnum = iMax(d->bookmarks->idEnum, id);
        insertId_Bookmarks_(d->bookmarks, d->bm, id);
    }
    else {
        d->bm = NULL;
    }
}

static void handleKeyValue_BookmarkLoader_(void *context, const iString *table, const iString *key,
                                           const iTomlValue *tv) {
    iBookmarkLoader *d = context;
    iBookmark *bm = d->bm;
    if (bm) {
        iUnused(table); /* it's the current one */
        if (!cmp_String(key, "url") && tv->type == string_TomlType) {
            set_String(&bm->url, tv->value.string);
        }
        else if (!cmp_String(key, "title") && tv->type == string_TomlType) {
            set_String(&bm->title, tv->value.string);
        }
        else if (!cmp_String(key, "tags") && tv->type == string_TomlType) {
            set_String(&bm->tags, tv->value.string);
        }
        else if (!cmp_String(key, "icon") && tv->type == int64_TomlType) {
            bm->icon = (iChar) tv->value.int64;
        }
        else if (!cmp_String(key, "created") && tv->type == int64_TomlType) {
            initSeconds_Time(&bm->when, tv->value.int64);
        }
        else if (!cmp_String(key, "parent") && tv->type == int64_TomlType) {
            bm->parentId = tv->value.int64;
        }
        else if (!cmp_String(key, "order") && tv->type == int64_TomlType) {
            bm->order = tv->value.int64;
        }
    }
    else if (!cmp_String(key, "recentfolder") && tv->type == int64_TomlType) {
        d->bookmarks->recentFolderId = tv->value.int64;
    }
}

static void init_BookmarkLoader(iBookmarkLoader *d, iBookmarks *bookmarks) {
    d->toml = new_TomlParser();
    setHandlers_TomlParser(d->toml, handleTable_BookmarkLoader_, handleKeyValue_BookmarkLoader_, d);
    d->bookmarks = bookmarks;
    d->bm = NULL;
}

static void deinit_BookmarkLoader(iBookmarkLoader *d) {
    delete_TomlParser(d->toml);
}

static void load_BookmarkLoader(iBookmarkLoader *d, iFile *file) {
    if (!parse_TomlParser(d->toml, collect_String(readString_File(file)))) {
        fprintf(stderr, "[Bookmarks] syntax error(s) in %s\n", cstr_String(path_File(file)));
    }    
}

iDefineTypeConstructionArgs(BookmarkLoader, (iBookmarks *b), b)
    
/*----------------------------------------------------------------------------------------------*/

static iBool isMatchingParent_Bookmark_(void *context, const iBookmark *bm) {
    return bm->parentId == *(const uint32_t *) context;
}

void sort_Bookmarks(iBookmarks *d, uint32_t parentId, iBookmarksCompareFunc cmp) {
    lock_Mutex(d->mtx);
    iConstForEach(PtrArray, i, list_Bookmarks(d, cmp, isMatchingParent_Bookmark_, &parentId)) {
        iBookmark *bm = i.ptr;
        bm->order = index_PtrArrayConstIterator(&i) + 1;
    }
    unlock_Mutex(d->mtx);
}

void load_Bookmarks(iBookmarks *d, const char *dirPath) {
    clear_Bookmarks(d);
    /* Load new .ini bookmarks, if present. */
    iFile *f = iClob(newCStr_File(concatPath_CStr(dirPath, fileName_Bookmarks_)));
    if (!open_File(f, readOnly_FileMode | text_FileMode)) {
        /* As a fallback, try loading the v1.6 bookmarks file. */
        loadOldFormat_Bookmarks(d, dirPath);
        /* Old format has an implicit alphabetic sort order. */
        sort_Bookmarks(d, 0, cmpTitleAscending_Bookmark);
        return;
    }
    iBookmarkLoader loader;
    init_BookmarkLoader(&loader, d);
    load_BookmarkLoader(&loader, f);
    deinit_BookmarkLoader(&loader);
}

void save_Bookmarks(const iBookmarks *d, const char *dirPath) {
    lock_Mutex(d->mtx);
    iRegExp *remotePattern = iClob(new_RegExp("\\bremote\\b", caseSensitive_RegExpOption));
    iFile *f = newCStr_File(concatPath_CStr(dirPath, fileName_Bookmarks_));
    if (open_File(f, writeOnly_FileMode | text_FileMode)) {        
        iString *str = collectNew_String();
        format_String(str, "recentfolder = %u\n\n", d->recentFolderId);
        writeData_File(f, cstr_String(str), size_String(str));
        iConstForEach(Hash, i, &d->bookmarks) {
            const iBookmark *bm = (const iBookmark *) i.value;
            iRegExpMatch m;
            init_RegExpMatch(&m);
            if (matchString_RegExp(remotePattern, &bm->tags, &m)) {
                /* Remote bookmarks are not saved. */
                continue;
            }
            iBeginCollect();
            format_String(str,
                          "[%d]\n"
                          "url = \"%s\"\n"
                          "title = \"%s\"\n"
                          "tags = \"%s\"\n"
                          "icon = 0x%x\n"
                          "created = %.0f  # %s\n",
                          id_Bookmark(bm),
                          cstrCollect_String(quote_String(&bm->url, iFalse)),
                          cstrCollect_String(quote_String(&bm->title, iFalse)),
                          cstrCollect_String(quote_String(&bm->tags, iFalse)),
                          bm->icon,
                          seconds_Time(&bm->when),
                          cstrCollect_String(format_Time(&bm->when, "%Y-%m-%d")));
            if (bm->parentId) {
                appendFormat_String(str, "parent = %d\n", bm->parentId);
            }
            if (bm->order) {
                appendFormat_String(str, "order = %d\n", bm->order);
            }
            appendCStr_String(str, "\n");
            writeData_File(f, cstr_String(str), size_String(str));
            iEndCollect();
        }        
    }
    iRelease(f);
    unlock_Mutex(d->mtx);
}

static iRangei orderRange_Bookmarks_(const iBookmarks *d) {
    iRangei ord = { 0, 0 };
    iConstForEach(Hash, i, &d->bookmarks) {
        const iBookmark *bm = (const iBookmark *) i.value;
        if (isEmpty_Range(&ord)) {
            ord.start = bm->order;
            ord.end = bm->order + 1;
        }
        else {
            ord.start = iMin(ord.start, bm->order);
            ord.end   = iMax(ord.end, bm->order + 1);
        }
    }
    return ord;
}

uint32_t add_Bookmarks(iBookmarks *d, const iString *url, const iString *title, const iString *tags,
                       iChar icon) {
    lock_Mutex(d->mtx);
    iBookmark *bm = new_Bookmark();
    if (url) {
        set_String(&bm->url, canonicalUrl_String(url));
    }
    set_String(&bm->title, title);
    if (tags) {
        set_String(&bm->tags, tags);
    }
    bm->icon = icon;
    initCurrent_Time(&bm->when);
    const iRangei ord = orderRange_Bookmarks_(d);
    if (prefs_App()->addBookmarksToBottom) {
        bm->order = ord.end; /* Last in lists. */
    }
    else {
        bm->order = ord.start - 1; /* First in lists. */
    }
    insert_Bookmarks_(d, bm);
    unlock_Mutex(d->mtx);
    return id_Bookmark(bm);
}

iBool remove_Bookmarks(iBookmarks *d, uint32_t id) {
    lock_Mutex(d->mtx);
    iBookmark *bm = (iBookmark *) remove_Hash(&d->bookmarks, id);
    if (bm) {
        /* Remove all the contained bookmarks as well. */
        iConstForEach(PtrArray, i, list_Bookmarks(d, NULL, filterInsideFolder_Bookmark, bm)) {
            delete_Bookmark((iBookmark *) remove_Hash(&d->bookmarks, id_Bookmark(i.ptr)));
        }
        delete_Bookmark(bm);
    }
    unlock_Mutex(d->mtx);
    return bm != NULL;
}

iBool updateBookmarkIcon_Bookmarks(iBookmarks *d, const iString *url, iChar icon) {
    iBool changed = iFalse;
    lock_Mutex(d->mtx);
    const uint32_t id = findUrl_Bookmarks(d, url);
    if (id) {
        iBookmark *bm = get_Bookmarks(d, id);
        if (!hasTag_Bookmark(bm, remote_BookmarkTag) && !hasTag_Bookmark(bm, userIcon_BookmarkTag)) {
            if (icon != bm->icon) {
                bm->icon = icon;
                changed = iTrue;
            }
        }
    }
    unlock_Mutex(d->mtx);
    return changed;
}

void setRecentFolder_Bookmarks(iBookmarks *d, uint32_t folderId) {
    iBookmark *bm = get_Bookmarks(d, folderId);
    if (bm && isFolder_Bookmark(bm)) {
        d->recentFolderId = folderId;
    }
    else {
        d->recentFolderId = 0;
    }
}

iChar siteIcon_Bookmarks(const iBookmarks *d, const iString *url) {
    if (isEmpty_String(url)) {
        return 0;
    }
    static iRegExp *tagPattern_;
    if (!tagPattern_) {
        tagPattern_ = new_RegExp("\\b" userIcon_BookmarkTag "\\b", caseSensitive_RegExpOption);
    }
    const iRangecc urlRoot      = urlRoot_String(url);
    size_t         matchingSize = iInvalidSize; /* we'll pick the shortest matching */
    iChar          icon         = 0;
    lock_Mutex(d->mtx);
    iConstForEach(Hash, i, &d->bookmarks) {
        const iBookmark *bm = (const iBookmark *) i.value;
        iRegExpMatch m;
        init_RegExpMatch(&m);
        if (bm->icon && matchString_RegExp(tagPattern_, &bm->tags, &m)) {
            const iRangecc bmRoot = urlRoot_String(&bm->url);
            if (equalRangeCase_Rangecc(urlRoot, bmRoot)) {
                const size_t n = size_String(&bm->url);
                if (n < matchingSize) {
                    matchingSize = n;
                    icon = bm->icon;
                }
            }
        }
    }
    unlock_Mutex(d->mtx);
    return icon;
}

iBookmark *get_Bookmarks(iBookmarks *d, uint32_t id) {
    return (iBookmark *) value_Hash(&d->bookmarks, id);
}

void reorder_Bookmarks(iBookmarks *d, uint32_t id, int newOrder) {
    lock_Mutex(d->mtx);
    iForEach(Hash, i, &d->bookmarks) {
        iBookmark *bm = (iBookmark *) i.value;
        if (id_Bookmark(bm) == id) {
            bm->order = newOrder;
        }
        else if (bm->order >= newOrder) {
            bm->order++;
        }
    }
    unlock_Mutex(d->mtx);
}

iBool filterTagsRegExp_Bookmarks(void *regExp, const iBookmark *bm) {
    iRegExpMatch m;
    init_RegExpMatch(&m);
    return matchString_RegExp(regExp, &bm->tags, &m);
}

static iBool matchUrl_(void *url, const iBookmark *bm) {
    return equalCase_String(url, &bm->url);
}

uint32_t findUrl_Bookmarks(const iBookmarks *d, const iString *url) {
    /* TODO: O(n), boo */
    url = canonicalUrl_String(url);
    const iPtrArray *found = list_Bookmarks(d, NULL, matchUrl_, (void *) url);
    if (isEmpty_PtrArray(found)) return 0;
    return id_Bookmark(constFront_PtrArray(found));
}

uint32_t recentFolder_Bookmarks(const iBookmarks *d) {
    return d->recentFolderId;
}

const iPtrArray *list_Bookmarks(const iBookmarks *d, iBookmarksCompareFunc cmp,
                                iBookmarksFilterFunc filter, void *context) {
    lock_Mutex(d->mtx);
    iPtrArray *list = collectNew_PtrArray();
    iConstForEach(Hash, i, &d->bookmarks) {
        const iBookmark *bm = (const iBookmark *) i.value;
        if (!filter || filter(context, bm)) {
            pushBack_PtrArray(list, bm);
        }
    }
    unlock_Mutex(d->mtx);
    if (!cmp) cmp = cmpTimeDescending_Bookmark_;
    sort_Array(list, (int (*)(const void *, const void *)) cmp);
    return list;
}

size_t count_Bookmarks(const iBookmarks *d) {
    size_t n = 0;
    iConstForEach(Hash, i, &d->bookmarks) {
        if (!isFolder_Bookmark((const iBookmark *) i.value)) {
            n++;
        }
    }
    return n;
}

const iString *bookmarkListPage_Bookmarks(const iBookmarks *d, enum iBookmarkListType listType) {
    iString *str = collectNew_String();
    lock_Mutex(d->mtx);
    format_String(str,
                  "# ${bookmark.export.title.%s}\n\n",
                  listType == listByFolder_BookmarkListType ? "folder"
                  : listType == listByTag_BookmarkListType  ? "tag"
                                                            : "time");
    if (listType == listByFolder_BookmarkListType) {
        appendFormat_String(str,
                            "%s\n\n"
                            "${bookmark.export.saving}\n\n",
                            formatCStrs_Lang("bookmark.export.count.n", count_Bookmarks(d)));
    }
    else if (listType == listByTag_BookmarkListType) {
        appendFormat_String(str, "${bookmark.export.taginfo}\n\n");
    }
    iStringSet *tags = new_StringSet();
    const iPtrArray *bmList =
        list_Bookmarks(d,
                       listType == listByCreationTime_BookmarkListType ? cmpTimeDescending_Bookmark_
                       : listType == listByTag_BookmarkListType        ? cmpTitleAscending_Bookmark
                                                                       : cmpTree_Bookmark,
                       NULL, NULL);
    if (listType == listByFolder_BookmarkListType) {
        iConstForEach(PtrArray, i, bmList) {
            const iBookmark *bm = i.ptr;
            if (!isFolder_Bookmark(bm) && !bm->parentId) {
                appendFormat_String(str, "=> %s %s\n", cstr_String(&bm->url), cstr_String(&bm->title));
            }
        }
    }
    iConstForEach(PtrArray, i, bmList) {
        const iBookmark *bm = i.ptr;
        if (isFolder_Bookmark(bm)) {
            if (listType == listByFolder_BookmarkListType) {
                const int depth = depth_Bookmark(bm);
                appendFormat_String(str, "\n%s %s\n",
                                    depth == 0 ? "##" : "###", cstr_String(&bm->title));
            }
            continue;
        }
        if (listType == listByFolder_BookmarkListType && bm->parentId) {
            appendFormat_String(str, "=> %s %s\n", cstr_String(&bm->url), cstr_String(&bm->title));
        }
        else if (listType == listByCreationTime_BookmarkListType) {
            appendFormat_String(str, "=> %s %s - %s\n", cstr_String(&bm->url),
                                cstrCollect_String(format_Time(&bm->when, "%Y-%m-%d")),
                                cstr_String(&bm->title));
        }
        iRangecc tag = iNullRange;
        while (nextSplit_Rangecc(range_String(&bm->tags), " ", &tag)) {
            if (!isEmpty_Range(&tag)) {
                iString t;
                initRange_String(&t, tag);
                insert_StringSet(tags, &t);
                deinit_String(&t);
            }
        }
    }
    if (listType == listByTag_BookmarkListType) {
        iConstForEach(StringSet, t, tags) {
            const iString *tag = t.value;
            appendFormat_String(str, "\n## %s\n", cstr_String(tag));
            iConstForEach(PtrArray, i, bmList) {
                const iBookmark *bm = i.ptr;
                iRangecc bmTag = iNullRange;
                iBool isTagged = iFalse;
                while (nextSplit_Rangecc(range_String(&bm->tags), " ", &bmTag)) {
                    if (equal_Rangecc(bmTag, cstr_String(tag))) {
                        isTagged = iTrue;
                        break;
                    }
                }
                if (isTagged) {
                    appendFormat_String(
                        str, "=> %s %s\n", cstr_String(&bm->url), cstr_String(&bm->title));
                }
            }
        }
    }
    iRelease(tags);
    unlock_Mutex(d->mtx);
    if (listType == listByCreationTime_BookmarkListType) {
        appendCStr_String(str, "\n${bookmark.export.format.sub}\n");
    }
    else {
        appendFormat_String(str,
                            "\n${bookmark.export.format.linklines} "
                            "%s"
                            "${bookmark.export.format.otherlines}\n",
                            listType == listByFolder_BookmarkListType
                                ? "${bookmark.export.format.folders} "
                            : listType == listByTag_BookmarkListType
                                ? "${bookmark.export.format.tags} "
                                : "");
    }
    translate_Lang(str);
    return str;
}

static iBool isRemoteSource_Bookmark_(void *context, const iBookmark *d) {
    iUnused(context);
    return hasTag_Bookmark(d, remoteSource_BookmarkTag);
}

void remoteRequestFinished_Bookmarks_(iBookmarks *d, iGmRequest *req) {
    iUnused(d);
    postCommandf_App("bookmarks.request.finished req:%p", req);
}

void requestFinished_Bookmarks(iBookmarks *d, iGmRequest *req) {
    iBool found = iFalse;
    iForEach(PtrArray, i, &d->remoteRequests) {
        if (i.ptr == req) {
            remove_PtrArrayIterator(&i);
            found = iTrue;
            break;
        }
    }
    iAssert(found);
    /* Parse all links in the result. */
    if (isSuccess_GmStatusCode(status_GmRequest(req))) {
        iTime now;
        initCurrent_Time(&now);
        iRegExp *linkPattern = new_RegExp("^=>\\s*([^\\s]+)(\\s+(.*))?", 0);
        iString src;
        const iString *remoteTag = collectNewCStr_String("remote");
        initBlock_String(&src, body_GmRequest(req));
        iRangecc srcLine = iNullRange;
        while (nextSplit_Rangecc(range_String(&src), "\n", &srcLine)) {
            iRangecc line = srcLine;
            trimEnd_Rangecc(&line);
            iRegExpMatch m;
            init_RegExpMatch(&m);
            if (matchRange_RegExp(linkPattern, line, &m)) {
                const iRangecc url    = capturedRange_RegExpMatch(&m, 1);
                const iRangecc title  = capturedRange_RegExpMatch(&m, 3);
                iString *      urlStr = newRange_String(url);
                const iString *absUrl = canonicalUrl_String(absoluteUrl_String(url_GmRequest(req), urlStr));
                if (!findUrl_Bookmarks(d, absUrl)) {
                    iString *titleStr = newRange_String(title);
                    if (isEmpty_String(titleStr)) {
                        setRange_String(titleStr, urlHost_String(urlStr));
                    }
                    const uint32_t bmId = add_Bookmarks(d, absUrl, titleStr, remoteTag, 0x2913);
                    iBookmark *bm = get_Bookmarks(d, bmId);
                    bm->parentId = *(uint32_t *) userData_Object(req);
                    delete_String(titleStr);
                }
                delete_String(urlStr);
            }
        }
        deinit_String(&src);
        iRelease(linkPattern);
    }
    else {
        /* TODO: Show error? */
    }
    free(userData_Object(req));
    iRelease(req);
    if (isEmpty_PtrArray(&d->remoteRequests)) {
        postCommand_App("bookmarks.changed");
    }
}

void fetchRemote_Bookmarks(iBookmarks *d) {
    if (!isEmpty_PtrArray(&d->remoteRequests)) {
        return; /* Already ongoing. */
    }
    lock_Mutex(d->mtx);
    /* Remove all current remote bookmarks. */ {
        size_t numRemoved = 0;
        iForEach(Hash, i, &d->bookmarks) {
            iBookmark *bm = (iBookmark *) i.value;
            if (hasTag_Bookmark(bm, remote_BookmarkTag)) {
                remove_HashIterator(&i);
                delete_Bookmark(bm);
                numRemoved++;
            }
        }
        if (numRemoved) {
            postCommand_App("bookmarks.changed");
        }
    }
    iConstForEach(PtrArray, i, list_Bookmarks(d, NULL, isRemoteSource_Bookmark_, NULL)) {
        const iBookmark *bm   = i.ptr;
        iGmRequest *     req  = new_GmRequest(certs_App());
        uint32_t *       bmId = malloc(4);
        *bmId                 = id_Bookmark(bm);
        setUserData_Object(req, bmId);
        pushBack_PtrArray(&d->remoteRequests, req);
        setUrl_GmRequest(req, &bm->url);
        iConnect(GmRequest, req, finished, req, remoteRequestFinished_Bookmarks_);
        submit_GmRequest(req);
    }
    unlock_Mutex(d->mtx);
}
