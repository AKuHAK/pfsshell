import tarfile

def inspect_tar_archive(tar_file):
    with tarfile.open(tar_file, 'r') as archive:
        for member in archive.getmembers():
            # Check for compression information
            compression_info = member.pax_headers.get('SCHILY.extended-file-flags') if member.pax_headers else None
            if compression_info is not None:
                compression = compression_info
            else:
                compression = 'Uncompressed'

            # Determine archive format
            archive_format = 'Unknown'
            if member.type == tarfile.REGTYPE:
                archive_format = 'USTAR'
            elif member.type == tarfile.AREGTYPE:
                archive_format = 'GNU'
            elif member.type == tarfile.XHDTYPE:
                archive_format = 'PAX'

            print(f"File: {member.name}, Compression: {compression}, Format: {archive_format}")

inspect_tar_archive('test.tar')
