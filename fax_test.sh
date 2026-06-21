#!/bin/bash


./sip_fax "${mode[@]}" \
    --send-alt fine:"example_pages/fine.tiff" \
    --send-alt superfine:"example_pages/superfine.tiff" \
    --send-alt 300:"example_pages/300dpi.tiff" \
    --send-alt 400:"example_pages/400dpi.tiff" \
    --send-color "example_pages/color_fine.tiff" \
    --send-gray "example_pages/greyscale_fine.tiff" \
    "$@"
exit $?
