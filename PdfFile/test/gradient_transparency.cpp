/*
 * (c) Copyright Ascensio System SIA 2010-2023
 *
 * This program is a free software product. You can redistribute it and/or
 * modify it under the terms of the GNU Affero General Public License (AGPL)
 * version 3 as published by the Free Software Foundation. In accordance with
 * Section 7(a) of the GNU AGPL its Section 15 shall be amended to the effect
 * that Ascensio System SIA expressly excludes the warranty of non-infringement
 * of any third-party rights.
 *
 * This program is distributed WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR  PURPOSE. For
 * details, see the GNU AGPL at: http://www.gnu.org/licenses/agpl-3.0.html
 *
 * The  interactive user interfaces in modified source and object code versions
 * of the Program must display Appropriate Legal Notices, as required under
 * Section 5 of the GNU AGPL version 3.
 *
 * All the Product's GUI elements, including illustrations and icon sets, as
 * well as technical writing content are licensed under the terms of the
 * Creative Commons Attribution-ShareAlike 4.0 International. See the License
 * terms at http://creativecommons.org/licenses/by-sa/4.0/legalcode
 *
 */

// Generate two pages with linear and radial transparent gradients for mask validation.
#include "../PdfFile.h"
#include <memory>

int main()
{
    auto releaseFonts = [](NSFonts::IApplicationFonts* fonts) { fonts->Release(); };
    std::unique_ptr<NSFonts::IApplicationFonts, decltype(releaseFonts)> fonts(
        NSFonts::NSApplication::Create(), releaseFonts);
    CPdfFile document(fonts.get());
    document.CreatePdf();
    LONG colors[] = {0x08efeae0, 0x00efeae0};
    double stops[] = {0, 1};
    for (int page = 0; page < 2; ++page)
    {
        document.NewPage();
        document.put_Width(200);
        document.put_Height(100);
        for (int gradient = 0; gradient < 2; ++gradient)
        {
            if (gradient == 0)
                document.SetLinearGradient(10, 10, 90, 90);
            else
                document.SetRadialGradient(140, 50, 1, 140, 50, 40);
            document.put_BrushGradientColors(colors, stops, 2);
            document.PathCommandStart();
            document.PathCommandMoveTo(10 + gradient * 100, 10);
            document.PathCommandLineTo(90 + gradient * 100, 10);
            document.PathCommandLineTo(90 + gradient * 100, 90);
            document.PathCommandLineTo(10 + gradient * 100, 90);
            document.PathCommandClose();
            document.DrawPath(c_nWindingFillMode);
            document.PathCommandEnd();
        }
    }
    return document.SaveToFile(L"/tmp/gradient-transparency-test.pdf");
}
