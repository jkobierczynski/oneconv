// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
// Property IDs and object type identifiers (JCIDs) from [MS-ONE] 2.1.12 / 2.2,
// plus a few undocumented ones used by ink, math and recognition data.
#pragma once

#include <cstdint>

namespace oneconv::prop {

// clang-format off
constexpr uint32_t LayoutTightLayout            = 0x08001C00;
constexpr uint32_t PageWidth                    = 0x14001C01;
constexpr uint32_t PageHeight                   = 0x14001C02;
constexpr uint32_t OutlineElementChildLevel     = 0x0C001C03;
constexpr uint32_t Bold                         = 0x08001C04;
constexpr uint32_t Italic                       = 0x08001C05;
constexpr uint32_t Underline                    = 0x08001C06;
constexpr uint32_t Strikethrough                = 0x08001C07;
constexpr uint32_t Superscript                  = 0x08001C08;
constexpr uint32_t Subscript                    = 0x08001C09;
constexpr uint32_t Font                         = 0x1C001C0A;
constexpr uint32_t FontSize                     = 0x10001C0B;
constexpr uint32_t FontColor                    = 0x14001C0C;
constexpr uint32_t Highlight                    = 0x14001C0D;
constexpr uint32_t RgOutlineIndentDistance      = 0x1C001C12;
constexpr uint32_t BodyTextAlignment            = 0x0C001C13;
constexpr uint32_t OffsetFromParentHoriz        = 0x14001C14;
constexpr uint32_t OffsetFromParentVert         = 0x14001C15;
constexpr uint32_t NumberListFormat             = 0x1C001C1A;
constexpr uint32_t LayoutMaxWidth               = 0x14001C1B;
constexpr uint32_t LayoutMaxHeight              = 0x14001C1C;
constexpr uint32_t ContentChildNodes            = 0x24001C1F;
constexpr uint32_t ElementChildNodes            = 0x24001C20;
constexpr uint32_t RichEditTextUnicode          = 0x1C001C22;
constexpr uint32_t ListNodes                    = 0x24001C26;
constexpr uint32_t NotebookManagementEntityGuid = 0x1C001C30;
constexpr uint32_t LanguageId                   = 0x14001C3B;
constexpr uint32_t PictureContainer             = 0x20001C3F;
constexpr uint32_t ListFont                     = 0x1C001C52;
constexpr uint32_t TopologyCreationTimeStamp    = 0x18001C65;
constexpr uint32_t IsTitleTime                  = 0x08001C87;
constexpr uint32_t IsBoilerText                 = 0x08001C88;
constexpr uint32_t PageSize                     = 0x14001C8B;
constexpr uint32_t PortraitPage                 = 0x08001C8E;
constexpr uint32_t IsTitleText                  = 0x08001CB4;
constexpr uint32_t IsTitleDate                  = 0x08001CB5;
constexpr uint32_t ListRestart                  = 0x14001CB7;
constexpr uint32_t NotebookElementOrderingId    = 0x14001CB9;
constexpr uint32_t SectionColor                 = 0x14001CBE;
constexpr uint32_t ListSpacingMu                = 0x14001CCB;
constexpr uint32_t CachedTitleString            = 0x1C001CF3;
constexpr uint32_t TocChildren                  = 0x24001CF6;
constexpr uint32_t RichEditTextLangId           = 0x10001CFE;
constexpr uint32_t CreationTimeStamp            = 0x14001D09;
constexpr uint32_t CachedTitleStringFromPage    = 0x1C001D3C;
constexpr uint32_t RowCount                     = 0x14001D57;
constexpr uint32_t ColumnCount                  = 0x14001D58;
constexpr uint32_t TableBordersVisible          = 0x08001D5E;
constexpr uint32_t StructureElementChildNodes   = 0x24001D5F;
constexpr uint32_t ChildGraphSpaceElementNodes  = 0x2C001D63;
constexpr uint32_t TableColumnWidths            = 0x1C001D66;
constexpr uint32_t FolderChildFilename          = 0x1C001D6B;
constexpr uint32_t Author                       = 0x1C001D75;
constexpr uint32_t LastModifiedTimeStamp        = 0x18001D77;
constexpr uint32_t LastModifiedTime             = 0x14001D7A;
constexpr uint32_t EmbeddedFileContainer        = 0x20001D9B;
constexpr uint32_t EmbeddedFileName             = 0x1C001D9C;
constexpr uint32_t SourceFilepath               = 0x1C001D9D;
constexpr uint32_t IRecordMedia                 = 0x14001D24;
constexpr uint32_t ImageFilename                = 0x1C001DD7;
constexpr uint32_t IsDeletedGraphSpaceContent   = 0x00001DE9;
constexpr uint32_t IsBackground                 = 0x08001D13;
constexpr uint32_t PageLevel                    = 0x14001DFF;
constexpr uint32_t TextRunIndex                 = 0x1C001E12;
constexpr uint32_t TextRunFormatting            = 0x24001E13;
constexpr uint32_t Hyperlink                    = 0x08001E14;
constexpr uint32_t UnderlineType                = 0x0C001E15;
constexpr uint32_t Hidden                       = 0x08001E16;
constexpr uint32_t HyperlinkProtected           = 0x08001E19;
constexpr uint32_t WzHyperlinkUrl               = 0x1C001E20;
constexpr uint32_t TextRunIsEmbeddedObject      = 0x08001E22;
constexpr uint32_t CellBackgroundColor          = 0x14001E26;
constexpr uint32_t ImageAltText                 = 0x1C001E58;
constexpr uint32_t MathFormatting               = 0x08003401;
constexpr uint32_t InkDimensions                = 0x1C00340A;
constexpr uint32_t InkPath                      = 0x1C00340B;
constexpr uint32_t InkStrokeProperties          = 0x20003409;
constexpr uint32_t InkHeight                    = 0x1400340C;
constexpr uint32_t InkWidth                     = 0x1400340D;
constexpr uint32_t InkColor                     = 0x1400340F;
constexpr uint32_t InkPenTip                    = 0x0C003412;
constexpr uint32_t InkTransparency              = 0x0C003414;
constexpr uint32_t InkData                      = 0x20003415;
constexpr uint32_t InkStrokes                   = 0x24003416;
constexpr uint32_t InkBoundingBox               = 0x1C003418;
constexpr uint32_t InkScalingX                  = 0x14001C46;
constexpr uint32_t InkScalingY                  = 0x14001C47;
constexpr uint32_t PictureFileExtension         = 0x24003424;
constexpr uint32_t ParagraphStyle               = 0x2000342C;
constexpr uint32_t ParagraphSpaceBefore         = 0x1400342E;
constexpr uint32_t ParagraphSpaceAfter          = 0x1400342F;
constexpr uint32_t ParagraphLineSpacingExact    = 0x14003430;
constexpr uint32_t MathInlineObjectType         = 0x1400344F;
constexpr uint32_t MathInlineObjectCount        = 0x14003450;
constexpr uint32_t MathInlineObjectCol          = 0x0C003451;
constexpr uint32_t MathInlineObjectAlign        = 0x0C003452;
constexpr uint32_t MathInlineObjectChar         = 0x10003453;
constexpr uint32_t MathInlineObjectChar1        = 0x10003454;
constexpr uint32_t MathInlineObjectChar2        = 0x10003455;
constexpr uint32_t EmbeddedObjectType           = 0x14003457;
constexpr uint32_t TextRunDataObject            = 0x24003458;
constexpr uint32_t ParagraphStyleId             = 0x1C00345A;
constexpr uint32_t ActionItemType               = 0x10003463;
constexpr uint32_t NoteTagShape                 = 0x10003464;
constexpr uint32_t NoteTagHighlightColor        = 0x14003465;
constexpr uint32_t NoteTagTextColor             = 0x14003466;
constexpr uint32_t NoteTagPropertyStatus        = 0x14003467;
constexpr uint32_t NoteTagLabel                 = 0x1C003468;
constexpr uint32_t NoteTagCreated               = 0x1400346E;
constexpr uint32_t NoteTagCompleted             = 0x1400346F;
constexpr uint32_t ActionItemStatus             = 0x10003470;
constexpr uint32_t ReadingOrderRtl              = 0x08003476;
constexpr uint32_t ParagraphAlignment           = 0x0C003477;
constexpr uint32_t NoteTagDefinitionOid         = 0x20003488;
constexpr uint32_t NoteTagStates                = 0x04003489;  // ArrayOfPropertyValues of note tags
constexpr uint32_t TextExtendedAscii            = 0x1C003498;
constexpr uint32_t TextRunData                  = 0x40003499;
constexpr uint32_t SectionDisplayName           = 0x1C00349B;
constexpr uint32_t EmbeddedInkStartX            = 0x1400349E;
constexpr uint32_t EmbeddedInkStartY            = 0x1400349F;
constexpr uint32_t EmbeddedInkWidth             = 0x140034A0;
constexpr uint32_t EmbeddedInkHeight            = 0x140034A1;
constexpr uint32_t EmbeddedInkOffsetHoriz       = 0x140034A2;
constexpr uint32_t EmbeddedInkOffsetVert        = 0x140034A3;
constexpr uint32_t EmbeddedInkSpaceWidth        = 0x14001C27;
constexpr uint32_t EmbeddedInkSpaceHeight       = 0x14001C28;
constexpr uint32_t PictureWidth                 = 0x140034CD;
constexpr uint32_t PictureHeight                = 0x140034CE;
constexpr uint32_t PageRecognizedTextContainer  = 0x200035D7;
constexpr uint32_t ImageEmbedType               = 0x140035F2;
constexpr uint32_t ImageEmbeddedUrl             = 0x1C0035F3;
// clang-format on

}  // namespace oneconv::prop

namespace oneconv::jcid {

// clang-format off
constexpr uint32_t TocContainer                     = 0x00020001;
constexpr uint32_t PageSeriesNode                   = 0x00060008;
constexpr uint32_t SectionNode                      = 0x00060007;
constexpr uint32_t PageNode                         = 0x0006000B;
constexpr uint32_t OutlineNode                      = 0x0006000C;
constexpr uint32_t OutlineElementNode               = 0x0006000D;
constexpr uint32_t RichTextNode                     = 0x0006000E;
constexpr uint32_t ImageNode                        = 0x00060011;
constexpr uint32_t NumberListNode                   = 0x00060012;
constexpr uint32_t InkContainer                     = 0x00060014;
constexpr uint32_t OutlineGroup                     = 0x00060019;
constexpr uint32_t TableNode                        = 0x00060022;
constexpr uint32_t TableRowNode                     = 0x00060023;
constexpr uint32_t TableCellNode                    = 0x00060024;
constexpr uint32_t TitleNode                        = 0x0006002C;
constexpr uint32_t PageMetadata                     = 0x00020030;
constexpr uint32_t SectionMetadata                  = 0x00020031;
constexpr uint32_t EmbeddedFileNode                 = 0x00060035;
constexpr uint32_t PageManifestNode                 = 0x00060037;
constexpr uint32_t EmbeddedFileContainer            = 0x00080036;
constexpr uint32_t PictureContainer                 = 0x00080039;
constexpr uint32_t InkDataNode                      = 0x0002003B;
constexpr uint32_t NoteTagSharedDefinitionContainer = 0x00120043;
constexpr uint32_t InkStrokeNode                    = 0x00020047;
constexpr uint32_t StrokePropertiesNode             = 0x00120048;
constexpr uint32_t ParagraphStyleObject             = 0x0012004D;
constexpr uint32_t IFrameNode                       = 0x00060058;
// clang-format on

}  // namespace oneconv::jcid
