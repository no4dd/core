# Transparent gradient regression

`CDictObject::Add` refuses to attach an already-owned direct dictionary. The transparency group must be indirect because pages and gradient soft-mask forms share it. Otherwise PDF export retains gradient colours but writes a mask without `/Group`.

The standalone fixture draws linear and radial gradients on two pages. It requires no input document, fonts or network access. Build the changed component using the DocumentServer development container first (`docker exec eo make core/docbuilder`), then from the DocumentServer root:

```sh
docker exec eo sh -c 'g++ -std=c++17 /develop/core/PdfFile/test/gradient_transparency.cpp -L/var/www/euro-office/documentserver/server/FileConverter/bin -Wl,-rpath,/var/www/euro-office/documentserver/server/FileConverter/bin -lPdfFile -lgraphics -o /tmp/gradient-transparency-test && /tmp/gradient-transparency-test'
docker cp eo:/tmp/gradient-transparency-test.pdf /tmp/gradient-transparency-test.pdf
python3 core/PdfFile/test/check_gradient_transparency.py /tmp/gradient-transparency-test.pdf 4
```

The validation script requires Python 3 and qpdf. It follows actual PDF references and verifies each soft-mask form has its transparency group and blending colour space. It also accepts a saved Office PDF: the original failing Slides export is rejected for its missing group; the same file exported after the fix passes. Both EuroOffice and Poppler should render the resulting transparency without soft-mask errors.
