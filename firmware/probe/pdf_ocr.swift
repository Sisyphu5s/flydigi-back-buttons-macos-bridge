// pdf_ocr.swift — 用 macOS Vision 把（扫描版）PDF 逐页 OCR 成文本
// 用法: ./pdf_ocr <file.pdf> [起始页=0] [结束页]
import Foundation
import PDFKit
import Vision
import AppKit

let args = CommandLine.arguments
guard args.count > 1 else { print("用法: pdf_ocr <file.pdf> [起始页] [结束页]"); exit(2) }
let path = args[1]
let start = args.count > 2 ? (Int(args[2]) ?? 0) : 0
let end = args.count > 3 ? (Int(args[3]) ?? Int.max) : Int.max
guard let doc = PDFDocument(url: URL(fileURLWithPath: path)) else { print("打不开 PDF"); exit(1) }
print("# \(path) 共 \(doc.pageCount) 页；OCR 第 \(start + 1)..\(min(end, doc.pageCount)) 页")

for i in start..<min(end, doc.pageCount) {
    guard let page = doc.page(at: i) else { continue }
    let rect = page.bounds(for: .mediaBox)
    let scale: CGFloat = args.count > 4 ? (CGFloat(Double(args[4]) ?? 2.5)) : 2.5
    let size = NSSize(width: rect.width * scale, height: rect.height * scale)
    let img = NSImage(size: size)
    img.lockFocus()
    NSColor.white.setFill()
    NSRect(origin: .zero, size: size).fill()
    if let ctx = NSGraphicsContext.current?.cgContext {
        ctx.scaleBy(x: scale, y: scale)
        page.draw(with: .mediaBox, to: ctx)
    }
    img.unlockFocus()
    if args.count > 5 && args[5] == "png" {
        if let tiff = img.tiffRepresentation, let rep = NSBitmapImageRep(data: tiff),
           let png = rep.representation(using: .png, properties: [:]) {
            let out = "/Users/retro/.hermes/cache/scratch/page_\(i + 1).png"
            try? png.write(to: URL(fileURLWithPath: out))
            print("wrote \(out)")
        }
        continue
    }
    guard let tiff = img.tiffRepresentation,
          let rep = NSBitmapImageRep(data: tiff),
          let cg = rep.cgImage else { continue }
    let req = VNRecognizeTextRequest()
    req.recognitionLanguages = ["zh-Hans", "en-US"]
    req.recognitionLevel = .accurate
    req.usesLanguageCorrection = true
    do {
        try VNImageRequestHandler(cgImage: cg, options: [:]).perform([req])
    } catch { print("=== page \(i + 1) === (OCR 失败 \(error))"); continue }
    print("=== page \(i + 1) ===")
    for obs in (req.results ?? []) {
        if let c = obs.topCandidates(1).first { print(c.string) }
    }
}
