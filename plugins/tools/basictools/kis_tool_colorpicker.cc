/*
 *  Copyright (c) 1999 Matthias Elter <me@kde.org>
 *  Copyright (c) 2002 Patrick Julien <freak@codepimps.org>
 *  Copyright (c) 2010 Lukáš Tvrdý <lukast.dev@gmail.com>
 *  Copyright (c) 2018 Emmet & Eoin O'Neill <emmetoneill.pdx@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "kis_tool_colorpicker.h"

#include <boost/thread/locks.hpp>
#include <QMessageBox>
#include "kis_cursor.h"
#include "KisDocument.h"
#include "kis_canvas2.h"
#include "KisReferenceImagesLayer.h"
#include "KoCanvasBase.h"
#include "kis_random_accessor_ng.h"
#include "KoResourceServerProvider.h"
#include <KoMixColorsOp.h>
#include "kis_wrapped_rect.h"
#include "kis_tool_utils.h"

namespace
{
// GUI ComboBox index constants
const int SAMPLE_MERGED = 0;
}

KisToolColorPicker::KisToolColorPicker(KoCanvasBase *canvas)
    : KisTool(canvas, KisCursor::pickerCursor()),
      m_config(new KisToolUtils::ColorPickerConfig)
{
    setObjectName("tool_colorpicker");
    m_isActivated = false;
    m_optionsWidget = 0;
    m_pickedColor = KoColor();
}

KisToolColorPicker::~KisToolColorPicker()
{
    if (m_isActivated) {
        m_config->save(m_toolActivationSource == KisTool::DefaultActivation);
    }
}

void KisToolColorPicker::paint(QPainter &gc, const KoViewConverter &converter)
{
    Q_UNUSED(gc);
    Q_UNUSED(converter);
}

void KisToolColorPicker::activate(ToolActivation activation, const QSet<KoShape*> &shapes)
{
    m_isActivated = true;
    m_toolActivationSource = activation;
    m_config->load(m_toolActivationSource == KisTool::DefaultActivation);
    updateOptionWidget();

    KisTool::activate(activation, shapes);
}

void KisToolColorPicker::deactivate()
{
    m_config->save(m_toolActivationSource == KisTool::DefaultActivation);
    m_isActivated = false;
    KisTool::deactivate();
}

void KisToolColorPicker::pickColor(const QPointF &pos)
{
    if (m_colorPickerDelayTimer.isActive()) {
        return;
    }
    else {
        m_colorPickerDelayTimer.setSingleShot(true);
        m_colorPickerDelayTimer.start(100);
    }

    QScopedPointer<boost::lock_guard<KisImage>> imageLocker;

    m_pickedColor.setOpacity(0.0);

    if (m_optionsWidget->cmbSources->currentIndex() == SAMPLE_MERGED) {
        auto *kisCanvas = dynamic_cast<KisCanvas2 *>(canvas());
        KIS_SAFE_ASSERT_RECOVER_RETURN(kisCanvas);
        KisReferenceImagesLayer *referenceImageLayer =
            kisCanvas->imageView()->document()->referenceImagesLayer();

        if (referenceImageLayer) {
            QColor color = referenceImageLayer->getPixel(pos);
            if (color.isValid()) {
                m_pickedColor.fromQColor(color);
            }
        }
    }

    if (m_pickedColor.opacityU8() == OPACITY_TRANSPARENT_U8) {
        KisPaintDeviceSP dev;

        if (m_optionsWidget->cmbSources->currentIndex() != SAMPLE_MERGED &&
            currentNode() && currentNode()->colorPickSourceDevice()) {
            dev = currentNode()->colorPickSourceDevice();
        }
        else {
            imageLocker.reset(new boost::lock_guard<KisImage>(*currentImage()));
            dev = currentImage()->projection();
        }

        // Color sampling radius.
        if (m_config->radius == 1) {
            QPoint realPos = pos.toPoint();
            if (currentImage()->wrapAroundModePermitted()) {
                realPos = KisWrappedRect::ptToWrappedPt(realPos, currentImage()->bounds());
            }

            dev->pixel(realPos.x(), realPos.y(), &m_pickedColor);
        }
        else {
            const KoColorSpace *cs = dev->colorSpace();
            int pixelSize = cs->pixelSize();

            quint8 *dstColor = new quint8[pixelSize];
            QVector<const quint8 *> pixels;

            KisRandomConstAccessorSP accessor = dev->createRandomConstAccessorNG(0, 0);

            for (int y = -m_config->radius; y <= m_config->radius; y++) {
                for (int x = -m_config->radius; x <= m_config->radius; x++) {
                    if (((x * x) + (y * y)) < m_config->radius * m_config->radius) {

                        QPoint realPos(pos.x() + x, pos.y() + y);

                        if (currentImage()->wrapAroundModePermitted()) {
                            realPos = KisWrappedRect::ptToWrappedPt(realPos, currentImage()->bounds());
                        }

                        accessor->moveTo(realPos.x(), realPos.y());
                        pixels << accessor->oldRawData();
                    }
                }
            }

            const quint8 **cpixels = const_cast<const quint8 **>(pixels.constData());
            cs->mixColorsOp()->mixColors(cpixels, pixels.size(), dstColor);

            m_pickedColor = KoColor(dstColor, cs);

            delete[] dstColor;
        }

        // Color blending.
        if(m_config->blend < 100){
            //Scale from 0..100% to 0..255 range for mixOp weights.
            quint8 blendScaled = static_cast<quint8>(m_config->blend * 2.55f);

            KoColor previousColor = canvas()->resourceManager()->foregroundColor();

            const quint8 *colors[2];
            colors[0] = previousColor.data();
            colors[1] = m_pickedColor.data();
            qint16 weights[2];
            weights[0] = 255 - blendScaled;
            weights[1] = blendScaled;

            const KoMixColorsOp *mixOp = dev->colorSpace()->mixColorsOp();
            mixOp->mixColors(colors, weights, 2, m_pickedColor.data());
        }

        m_pickedColor.convertTo(dev->compositionSourceColorSpace());

        if (m_config->updateColor &&
                m_pickedColor.opacityU8() != OPACITY_TRANSPARENT_U8) {

            KoColor publicColor = m_pickedColor;
            publicColor.setOpacity(OPACITY_OPAQUE_U8);

            if (m_config->toForegroundColor) {
                canvas()->resourceManager()->setResource(KoCanvasResourceManager::ForegroundColor, publicColor);
            }
            else {
                canvas()->resourceManager()->setResource(KoCanvasResourceManager::BackgroundColor, publicColor);
            }
        }
    }
}

void KisToolColorPicker::beginPrimaryAction(KoPointerEvent *event)
{
    bool sampleMerged = m_optionsWidget->cmbSources->currentIndex() == SAMPLE_MERGED;
    if (!sampleMerged) {
        if (!currentNode()) {
            QMessageBox::information(0, i18nc("@title:window", "Krita"), i18n("Cannot pick a color as no layer is active."));
            event->ignore();
            return;
        }
        if (!currentNode()->visible()) {
            QMessageBox::information(0, i18nc("@title:window", "Krita"), i18n("Cannot pick a color as the active layer is not visible."));
            event->ignore();
            return;
        }
    }

    QPoint pos = convertToImagePixelCoordFloored(event);
    // Color picking has to start in the visible part of the layer
    if (!currentImage()->bounds().contains(pos) &&
        !currentImage()->wrapAroundModePermitted()) {
        event->ignore();
        return;
    }

    setMode(KisTool::PAINT_MODE);
    pickColor(pos);
    displayPickedColor();
}

void KisToolColorPicker::continuePrimaryAction(KoPointerEvent *event)
{
    CHECK_MODE_SANITY_OR_RETURN(KisTool::PAINT_MODE);

    QPoint pos = convertToImagePixelCoordFloored(event);
    pickColor(pos);
    displayPickedColor();
}

void KisToolColorPicker::endPrimaryAction(KoPointerEvent *event)
{
    Q_UNUSED(event);
    CHECK_MODE_SANITY_OR_RETURN(KisTool::PAINT_MODE);

    if (m_config->addPalette) {
        KoColorSetEntry ent;
        ent.color = m_pickedColor;
        // We don't ask for a name, too intrusive here

        KoColorSet *palette = m_palettes.at(m_optionsWidget->cmbPalette->currentIndex());
        palette->add(ent);

        if (!palette->save()) {
            QMessageBox::critical(0, i18nc("@title:window", "Krita"), i18n("Cannot write to palette file %1. Maybe it is read-only.", palette->filename()));
        }
    }
}

struct PickedChannel {
    QString name;
    QString valueText;
};

void KisToolColorPicker::displayPickedColor()
{
    if (m_pickedColor.data() && m_optionsWidget) {

        QList<KoChannelInfo *> channels = m_pickedColor.colorSpace()->channels();
        m_optionsWidget->listViewChannels->clear();

        QVector<PickedChannel> pickedChannels;
        for (int i = 0; i < channels.count(); ++i) {
            pickedChannels.append(PickedChannel());
        }

        for (int i = 0; i < channels.count(); ++i) {

            PickedChannel pc;
            pc.name = channels[i]->name();

            if (m_config->normaliseValues) {
                pc.valueText = m_pickedColor.colorSpace()->normalisedChannelValueText(m_pickedColor.data(), i);
            } else {
                pc.valueText = m_pickedColor.colorSpace()->channelValueText(m_pickedColor.data(), i);
            }

            pickedChannels[channels[i]->displayPosition()] = pc;

        }

        Q_FOREACH (const PickedChannel &pc, pickedChannels) {
            QTreeWidgetItem *item = new QTreeWidgetItem(m_optionsWidget->listViewChannels);
            item->setText(0, pc.name);
            item->setText(1, pc.valueText);
        }
    }
}

QWidget* KisToolColorPicker::createOptionWidget()
{
    m_optionsWidget = new ColorPickerOptionsWidget(0);
    m_optionsWidget->setObjectName(toolId() + " option widget");
    m_optionsWidget->listViewChannels->setSortingEnabled(false);

    // See https://bugs.kde.org/show_bug.cgi?id=316896
    QWidget *specialSpacer = new QWidget(m_optionsWidget);
    specialSpacer->setObjectName("SpecialSpacer");
    specialSpacer->setFixedSize(0, 0);
    m_optionsWidget->layout()->addWidget(specialSpacer);

    // Initialize blend KisSliderSpinBox
    m_optionsWidget->blend->setRange(0,100);

    updateOptionWidget();

    connect(m_optionsWidget->cbUpdateCurrentColor, SIGNAL(toggled(bool)), SLOT(slotSetUpdateColor(bool)));
    connect(m_optionsWidget->cbNormaliseValues, SIGNAL(toggled(bool)), SLOT(slotSetNormaliseValues(bool)));
    connect(m_optionsWidget->cbPalette, SIGNAL(toggled(bool)),
            SLOT(slotSetAddPalette(bool)));
    connect(m_optionsWidget->radius, SIGNAL(valueChanged(int)),
            SLOT(slotChangeRadius(int)));
    connect(m_optionsWidget->blend, SIGNAL(valueChanged(int)),
            SLOT(slotChangeBlend(int)));
    connect(m_optionsWidget->cmbSources, SIGNAL(currentIndexChanged(int)),
            SLOT(slotSetColorSource(int)));

    KoResourceServer<KoColorSet> *srv = KoResourceServerProvider::instance()->paletteServer();

    if (!srv) {
        return m_optionsWidget;
    }

    QList<KoColorSet*> palettes = srv->resources();

    Q_FOREACH (KoColorSet *palette, palettes) {
        if (palette) {
            m_optionsWidget->cmbPalette->addSqueezedItem(palette->name());
            m_palettes.append(palette);
        }
    }

    return m_optionsWidget;
}

void KisToolColorPicker::updateOptionWidget()
{
    if (!m_optionsWidget) return;

    m_optionsWidget->cbNormaliseValues->setChecked(m_config->normaliseValues);
    m_optionsWidget->cbUpdateCurrentColor->setChecked(m_config->updateColor);
    m_optionsWidget->cmbSources->setCurrentIndex(SAMPLE_MERGED + !m_config->sampleMerged);
    m_optionsWidget->cbPalette->setChecked(m_config->addPalette);
    m_optionsWidget->radius->setValue(m_config->radius);
    m_optionsWidget->blend->setValue(m_config->blend);
}

void KisToolColorPicker::setToForeground(bool newValue)
{
    m_config->toForegroundColor = newValue;
    emit toForegroundChanged();
}

bool KisToolColorPicker::toForeground() const
{
    return m_config->toForegroundColor;
}

void KisToolColorPicker::slotSetUpdateColor(bool state)
{
    m_config->updateColor = state;
}

void KisToolColorPicker::slotSetNormaliseValues(bool state)
{
    m_config->normaliseValues = state;
    displayPickedColor();
}

void KisToolColorPicker::slotSetAddPalette(bool state)
{
    m_config->addPalette = state;
}

void KisToolColorPicker::slotChangeRadius(int value)
{
    m_config->radius = value;
}

void KisToolColorPicker::slotChangeBlend(int value)
{
    m_config->blend = value;
}

void KisToolColorPicker::slotSetColorSource(int value)
{
    m_config->sampleMerged = value == SAMPLE_MERGED;
}

void KisToolColorPicker::slotAddPalette(KoResource *resource)
{
    KoColorSet *palette = dynamic_cast<KoColorSet*>(resource);
    if (palette) {
        m_optionsWidget->cmbPalette->addSqueezedItem(palette->name());
        m_palettes.append(palette);
    }
}
